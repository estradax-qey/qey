#ifndef QEY_DB_H_
#define QEY_DB_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include "qey/file.h"
#include "qey/log.h"
#include "qey/status.h"
#include "qey/storage.h"

namespace qey {

// When a persistent Db flushes its log to stable storage.
enum class SyncPolicy {
  // After every write. A write that returned OK survives a crash.
  kAlways,
  // At most OpenOptions::sync_interval after a write, by a background thread
  // (or by the owner calling Db::SyncIfDue()), and also from writes that
  // arrive once the interval has elapsed. A crash or power loss can lose
  // writes from up to about one interval before it; the log stays intact.
  kInterval,
  // Only when Db::Sync() is called. Faster, but recent writes can be lost.
  kManual,
};

struct OpenOptions {
  SyncPolicy sync = SyncPolicy::kAlways;
  // For SyncPolicy::kInterval. Must be positive.
  std::chrono::milliseconds sync_interval{1000};
  // For SyncPolicy::kInterval: whether the Db runs a background thread that
  // syncs every interval. Without it, data is synced only when a write
  // arrives after the interval, or when the owner calls Db::SyncIfDue().
  bool sync_thread = true;
  // What to do about a damaged tail of the log. See Replay().
  Recovery recovery = Recovery::kTruncate;
  // Compaction rewrites the log to hold only live data, dropping overwritten
  // and deleted records. When enabled, it runs on its own before a write once
  // the log is larger than `compact_min_bytes` and `compact_ratio` times its
  // size after the last compaction (or the size of the live data, at open).
  // It blocks other callers while it runs. A failed automatic compaction is
  // ignored, since it does not affect the write; call Compact() to see errors.
  bool auto_compact = true;
  uint64_t compact_min_bytes = 4u << 20;
  uint32_t compact_ratio = 2;
  // Clock overrides, for tests. Empty means the real clocks.
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<std::chrono::system_clock::time_point()> wall_now;
};

// What Db::Open() found in the log.
struct RecoveryInfo {
  size_t records_replayed = 0;
  // Bytes of a damaged tail that were discarded. Non-zero after a crash
  // during a write, or if the log was damaged; see Replay().
  uint64_t dropped_bytes = 0;
};

// A key-value store held in memory, optionally backed by a log file for
// durability. All methods are thread-safe.
//
// Create an in-memory store with Db(), or a persistent one with Open().
// Operations that modify the store return a Status (or StatusOr) so that
// failures can be reported. An in-memory Db never fails. In a persistent Db a
// change is logged before it is applied, so a failed write leaves the store
// unchanged. If the log cannot be repaired after a failed write, the Db stops
// accepting writes and returns kIoError until it is reopened.
//
// Expired keys are removed lazily the next time they are accessed. Keys that
// are never accessed again are reclaimed by calling Sweep(), which the owner
// should do periodically.
class Db {
 public:
  // The expiry state of a key, as reported by GetTtl().
  enum class TtlState {
    kMissing,   // The key does not exist (or has expired).
    kNoExpiry,  // The key exists and never expires.
    kExpiring,  // The key exists and expires after `remaining`.
  };

  struct TtlInfo {
    TtlState state;
    // Time until expiry, rounded down. Only meaningful for kExpiring.
    std::chrono::milliseconds remaining{0};
  };

  // The longest TTL accepted, about 100 years. Longer ones are rejected, so
  // that time arithmetic cannot overflow.
  static constexpr std::chrono::milliseconds kMaxTtl{
      std::chrono::hours(24 * 365 * 100)};

  using Clock = std::chrono::steady_clock;
  using NowFn = std::function<Clock::time_point()>;
  using WallNowFn = std::function<std::chrono::system_clock::time_point()>;

  // Creates an in-memory store that reads time from the steady clock.
  Db();

  // Creates an in-memory store that reads time from `now`. Intended for tests.
  explicit Db(NowFn now);

  // Opens the persistent store at `path`, creating it if it does not exist,
  // and loads its contents. The file is locked until the Db is destroyed.
  // Fails with kUnavailable if it is already open, kCorruption if the log is
  // damaged beyond a torn tail (see Replay()), or kIoError if the file cannot
  // be used.
  static StatusOr<std::unique_ptr<Db>> Open(std::string_view path,
                                            OpenOptions options = {});

  // Like Open(path), but on custom `storage`, which the Db takes over.
  static StatusOr<std::unique_ptr<Db>> Open(std::unique_ptr<Storage> storage,
                                            OpenOptions options = {});

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Stops the background sync thread, if any, and syncs any writes that are
  // not yet on disk. The caller must make sure no other thread is still using
  // the Db.
  ~Db();

  // Sets `key` to `value`, replacing any existing value and TTL. If `ttl` is
  // set, the key expires after that duration; otherwise it never expires. A
  // non-positive `ttl` makes the key expire immediately. Fails with
  // kInvalidArgument, changing nothing, if `ttl` exceeds kMaxTtl.
  Status Set(std::string_view key, std::string_view value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);

  // Returns the value of `key`, or std::nullopt if it is missing or expired.
  std::optional<std::string> Get(std::string_view key);

  // Removes `key`. Returns true if a live key was removed.
  StatusOr<bool> Del(std::string_view key);

  // Adds `delta` to the integer stored at `key` and returns the new value. A
  // missing key is treated as 0. The key's TTL is preserved. Fails with
  // kNotInteger if the stored value is not a base-10 64-bit integer, or
  // kOutOfRange if the result overflows; the stored value is unchanged on
  // failure.
  StatusOr<int64_t> Incr(std::string_view key, int64_t delta = 1);

  // Like Incr(), but subtracts `delta`.
  StatusOr<int64_t> Decr(std::string_view key, int64_t delta = 1);

  // Removes expired keys, at most `limit` of them, and returns how many were
  // removed. The limit bounds how long the store is locked; call again if the
  // result equals `limit`.
  size_t Sweep(size_t limit = std::numeric_limits<size_t>::max());

  // Returns the number of stored keys, including expired keys that have not
  // been removed yet.
  size_t Size();

  // Returns true if `key` exists and has not expired.
  bool Exists(std::string_view key);

  // Makes `key` expire after `ttl`, replacing any existing TTL. Returns true
  // if the key exists. A non-positive `ttl` expires the key immediately. Fails
  // with kInvalidArgument, changing nothing, if `ttl` exceeds kMaxTtl.
  StatusOr<bool> Expire(std::string_view key, std::chrono::milliseconds ttl);

  // Removes the TTL from `key`. Returns true if the key had a TTL.
  StatusOr<bool> Persist(std::string_view key);

  // Reports the expiry state of `key`.
  TtlInfo GetTtl(std::string_view key);

  // Rewrites the log to hold only the live keys, then removes expired keys
  // from memory. The log is replaced atomically: if this fails, the old log
  // is intact and in use. Does nothing for an in-memory Db.
  Status Compact();

  // Flushes the log to stable storage. Needed only with SyncPolicy::kManual
  // or kInterval. Does nothing for an in-memory Db.
  Status Sync();

  // With SyncPolicy::kInterval, flushes the log if there are unsynced writes
  // and the interval has passed since the last sync. For owners that run
  // their own timer instead of the background thread. Does nothing under
  // other policies.
  Status SyncIfDue();

  // Reports what Open() found in the log. Zeroes for an in-memory Db.
  RecoveryInfo recovery_info() const { return recovery_info_; }

 private:
  struct Entry {
    std::string value;
    std::optional<Clock::time_point> expires_at;
  };

  using Map = std::unordered_map<std::string, Entry>;

  Db(NowFn now, WallNowFn wall_now);

  // Replays the log into memory and repairs its tail. Used by Open().
  Status Recover(Recovery recovery);

  // Compacts the log if it has outgrown the thresholds in `options_`. Call
  // before looking anything up, while memory and log agree. Requires `mu_`.
  void MaybeCompactLocked();

  // Rewrites the log from memory, omitting expired keys. Requires `mu_` held.
  Status CompactLocked();

  // Sets the size at which the next automatic compaction happens, given the
  // log size right after a compaction (or the live data size).
  void SetCompactThreshold(uint64_t base_size);

  // Returns OK, or an error if an earlier I/O failure left the log in a state
  // that cannot be trusted. Requires `mu_` held.
  Status CheckHealthyLocked() const;

  // Records `cause` as the reason the Db stopped accepting writes. Keeps the
  // first one. Requires `mu_` held.
  void FailLocked(const Status& cause);

  // Syncs the log and updates the bookkeeping. A failure puts the Db in the
  // failed state. Requires `mu_` held.
  Status SyncLocked();

  // Body of the background sync thread.
  void SyncThreadMain();

  // Appends `record` to the log and syncs it according to the policy. Does
  // nothing without a log. Requires `mu_` held.
  Status LogLocked(const Record& record);

  // Applies a record read from the log, without logging it. Requires `mu_`
  // held (or exclusive access during Open()).
  void ApplyRecordLocked(Record&& record);

  // Converts between the monotonic clock used in memory and the Unix time
  // stored in the log.
  int64_t ToWallMs(Clock::time_point at);
  Clock::time_point FromWallMs(int64_t unix_ms);

  // Returns an iterator to the live entry for `key`, erasing it first if it
  // has expired. Returns map_.end() if there is none. Requires `mu_` held.
  Map::iterator FindLive(std::string_view key);

  // Sets or clears the expiry of the entry at `it`, keeping `expiry_` in sync.
  // Requires `mu_` held.
  void SetExpiryLocked(Map::iterator it, Clock::time_point at);
  void ClearExpiryLocked(Map::iterator it);

  // Removes the entry at `it` and its expiry. Requires `mu_` held.
  void EraseLocked(Map::iterator it);

  // Implements Incr() and Decr(). Requires `mu_` held.
  StatusOr<int64_t> AddLocked(std::string_view key, int64_t delta,
                              bool subtract);

  NowFn now_;
  WallNowFn wall_now_;
  std::mutex mu_;
  std::unique_ptr<Storage> storage_;  // Null for an in-memory Db.
  OpenOptions options_;
  uint64_t compact_threshold_ = 0;
  // Not OK once a log write or sync failed in a way that cannot be undone;
  // holds the first cause. Writes are refused from then on.
  Status failure_;
  bool unsynced_ = false;  // Records were appended but not synced.
  Clock::time_point last_sync_;
  std::thread sync_thread_;
  std::condition_variable stop_cv_;
  bool stopping_ = false;
  RecoveryInfo recovery_info_;
  Map map_;
  // Mirrors the expiry of every entry in `map_` that has one, ordered by
  // expiry time, so Sweep() only visits keys that are due.
  std::set<std::pair<Clock::time_point, std::string>> expiry_;
};

}  // namespace qey

#endif  // QEY_DB_H_
