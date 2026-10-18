#ifndef QEY_DB_H_
#define QEY_DB_H_

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qey {

// An in-memory key-value store. All methods are thread-safe.
//
// Expiry is lazy: an expired key is removed the next time it is accessed.
class Db {
 public:
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

 private:
  struct Entry {
    std::string value;
    std::optional<Clock::time_point> expires_at;
  };

  using Map = std::unordered_map<std::string, Entry>;

  // Returns an iterator to the live entry for `key`, erasing it first if it
  // has expired. Returns map_.end() if there is none. Requires `mu_` held.
  Map::iterator FindLive(std::string_view key);

  NowFn now_;
  std::mutex mu_;
  Map map_;
};

}  // namespace qey

#endif  // QEY_DB_H_
