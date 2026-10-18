#ifndef QEY_DB_H_
#define QEY_DB_H_

#include <chrono>
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
#include <unordered_map>
#include <utility>

#include "qey/file.h"
#include "qey/log.h"
#include "qey/status.h"

namespace qey {

// When a persistent Db flushes its log to stable storage.
enum class SyncPolicy {
  // After every write. A write that returned OK survives a crash.
  kAlways,
  // Only when Db::Sync() is called. Faster, but recent writes can be lost.
  kManual,
};

struct OpenOptions {
  SyncPolicy sync = SyncPolicy::kAlways;
  // What to do about a damaged tail of the log. See Replay().
  Recovery recovery = Recovery::kTruncate;
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

  using Clock = std::chrono::steady_clock;
  using NowFn = std::function<Clock::time_point()>;
  using WallNowFn = std::function<std::chrono::system_clock::time_point()>;

  // Creates an in-memory store that reads time from the steady clock.
  Db();

  // Creates an in-memory store that reads time from `now`. Intended for tests.
  explicit Db(NowFn now);

  // Opens the persistent store at `path`, creating it if it does not exist,
  // and loads its contents. Fails with kCorruption if the log is damaged
  // beyond a torn tail (see Replay()), or kIoError if the file cannot be used.
  static StatusOr<std::unique_ptr<Db>> Open(std::string_view path,
                                            OpenOptions options = {});

  // Like Open(path), but on an already open `file`, which the Db takes over.
  static StatusOr<std::unique_ptr<Db>> Open(std::unique_ptr<File> file,
                                            OpenOptions options = {});

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Sets `key` to `value`, replacing any existing value and TTL. If `ttl` is
  // set, the key expires after that duration; otherwise it never expires.
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
  // if the key exists. A non-positive `ttl` expires the key immediately.
  StatusOr<bool> Expire(std::string_view key, std::chrono::milliseconds ttl);

  // Removes the TTL from `key`. Returns true if the key had a TTL.
  StatusOr<bool> Persist(std::string_view key);

  // Reports the expiry state of `key`.
  TtlInfo GetTtl(std::string_view key);

  // Flushes the log to stable storage. Needed only with SyncPolicy::kManual.
  // Does nothing for an in-memory Db.
  Status Sync();

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
  std::unique_ptr<File> file_;  // Null for an in-memory Db.
  SyncPolicy sync_ = SyncPolicy::kAlways;
  bool failed_ = false;  // A log write failed and could not be undone.
  RecoveryInfo recovery_info_;
  Map map_;
  // Mirrors the expiry of every entry in `map_` that has one, ordered by
  // expiry time, so Sweep() only visits keys that are due.
  std::set<std::pair<Clock::time_point, std::string>> expiry_;
};

}  // namespace qey

#endif  // QEY_DB_H_
