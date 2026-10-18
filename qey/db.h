#ifndef QEY_DB_H_
#define QEY_DB_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "qey/status.h"

namespace qey {

// An in-memory key-value store. All methods are thread-safe.
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

  // Creates a store that reads time from the steady clock.
  Db();

  // Creates a store that reads time from `now`. Intended for tests.
  explicit Db(NowFn now);

  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Sets `key` to `value`, replacing any existing value and TTL. If `ttl` is
  // set, the key expires after that duration; otherwise it never expires.
  void Set(std::string_view key, std::string_view value,
           std::optional<std::chrono::milliseconds> ttl = std::nullopt);

  // Returns the value of `key`, or std::nullopt if it is missing or expired.
  std::optional<std::string> Get(std::string_view key);

  // Removes `key`. Returns true if a live key was removed.
  bool Del(std::string_view key);

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
  bool Expire(std::string_view key, std::chrono::milliseconds ttl);

  // Removes the TTL from `key`. Returns true if the key had a TTL.
  bool Persist(std::string_view key);

  // Reports the expiry state of `key`.
  TtlInfo GetTtl(std::string_view key);

 private:
  struct Entry {
    std::string value;
    std::optional<Clock::time_point> expires_at;
  };

  using Map = std::unordered_map<std::string, Entry>;

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
  std::mutex mu_;
  Map map_;
  // Mirrors the expiry of every entry in `map_` that has one, ordered by
  // expiry time, so Sweep() only visits keys that are due.
  std::set<std::pair<Clock::time_point, std::string>> expiry_;
};

}  // namespace qey

#endif  // QEY_DB_H_
