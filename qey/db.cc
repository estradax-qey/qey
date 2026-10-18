#include "qey/db.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qey {

Db::Db() : Db(&Clock::now) {}

Db::Db(NowFn now) : now_(std::move(now)) {}

void Db::Set(std::string_view key, std::string_view value,
             std::optional<std::chrono::milliseconds> ttl) {
  std::lock_guard<std::mutex> lock(mu_);
  Entry entry{std::string(value), std::nullopt};
  if (ttl.has_value()) entry.expires_at = now_() + *ttl;
  map_.insert_or_assign(std::string(key), std::move(entry));
}

std::optional<std::string> Db::Get(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return std::nullopt;
  return it->second.value;
}

bool Db::Del(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return false;
  map_.erase(it);
  return true;
}

StatusOr<int64_t> Db::Incr(std::string_view key, int64_t delta) {
  std::lock_guard<std::mutex> lock(mu_);
  return AddLocked(key, delta, /*subtract=*/false);
}

StatusOr<int64_t> Db::Decr(std::string_view key, int64_t delta) {
  std::lock_guard<std::mutex> lock(mu_);
  return AddLocked(key, delta, /*subtract=*/true);
}

bool Db::Exists(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  return FindLive(key) != map_.end();
}

bool Db::Expire(std::string_view key, std::chrono::milliseconds ttl) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return false;
  it->second.expires_at = now_() + ttl;
  return true;
}

bool Db::Persist(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end() || !it->second.expires_at.has_value()) return false;
  it->second.expires_at.reset();
  return true;
}

Db::TtlInfo Db::GetTtl(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return {TtlState::kMissing};
  const auto& expires_at = it->second.expires_at;
  if (!expires_at.has_value()) return {TtlState::kNoExpiry};
  return {TtlState::kExpiring,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              *expires_at - now_())};
}

StatusOr<int64_t> Db::AddLocked(std::string_view key, int64_t delta,
                                bool subtract) {
  auto it = FindLive(key);
  int64_t current = 0;
  if (it != map_.end()) {
    const std::string& s = it->second.value;
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, current);
    if (ec == std::errc::result_out_of_range) {
      return Status(StatusCode::kNotInteger, "value is out of int64 range");
    }
    if (ec != std::errc() || ptr != end) {
      return Status(StatusCode::kNotInteger, "value is not an integer");
    }
  }

  int64_t result;
  bool overflow = subtract ? __builtin_sub_overflow(current, delta, &result)
                           : __builtin_add_overflow(current, delta, &result);
  if (overflow) {
    return Status(StatusCode::kOutOfRange,
                  "increment or decrement would overflow");
  }

  std::string text = std::to_string(result);
  if (it != map_.end()) {
    it->second.value = std::move(text);
  } else {
    map_.insert_or_assign(std::string(key),
                          Entry{std::move(text), std::nullopt});
  }
  return result;
}

Db::Map::iterator Db::FindLive(std::string_view key) {
  auto it = map_.find(std::string(key));
  if (it == map_.end()) return it;
  const auto& expires_at = it->second.expires_at;
  if (expires_at.has_value() && now_() >= *expires_at) {
    map_.erase(it);
    return map_.end();
  }
  return it;
}

}  // namespace qey
