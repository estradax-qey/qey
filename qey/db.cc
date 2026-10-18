#include "qey/db.h"

#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qey {

Db::Db() : Db(&Clock::now) {}

Db::Db(NowFn now) : now_(std::move(now)) {}

Status Db::Set(std::string_view key, std::string_view value,
               std::optional<std::chrono::milliseconds> ttl) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.try_emplace(std::string(key)).first;
  ClearExpiryLocked(it);
  it->second.value = std::string(value);
  if (ttl.has_value()) SetExpiryLocked(it, now_() + *ttl);
  return Status::Ok();
}

std::optional<std::string> Db::Get(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return std::nullopt;
  return it->second.value;
}

StatusOr<bool> Db::Del(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return false;
  EraseLocked(it);
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

size_t Db::Sweep(size_t limit) {
  std::lock_guard<std::mutex> lock(mu_);
  const Clock::time_point now = now_();
  size_t removed = 0;
  while (removed < limit && !expiry_.empty() && expiry_.begin()->first <= now) {
    auto it = map_.find(expiry_.begin()->second);
    assert(it != map_.end());
    EraseLocked(it);
    ++removed;
  }
  return removed;
}

size_t Db::Size() {
  std::lock_guard<std::mutex> lock(mu_);
  return map_.size();
}

bool Db::Exists(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  return FindLive(key) != map_.end();
}

StatusOr<bool> Db::Expire(std::string_view key, std::chrono::milliseconds ttl) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return false;
  SetExpiryLocked(it, now_() + ttl);
  return true;
}

StatusOr<bool> Db::Persist(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end() || !it->second.expires_at.has_value()) return false;
  ClearExpiryLocked(it);
  return true;
}

Db::TtlInfo Db::GetTtl(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end()) return {TtlState::kMissing};
  const auto& expires_at = it->second.expires_at;
  if (!expires_at.has_value()) return {TtlState::kNoExpiry};
  return {TtlState::kExpiring,
          std::chrono::duration_cast<std::chrono::milliseconds>(*expires_at -
                                                                now_())};
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

void Db::SetExpiryLocked(Map::iterator it, Clock::time_point at) {
  ClearExpiryLocked(it);
  it->second.expires_at = at;
  expiry_.emplace(at, it->first);
}

void Db::ClearExpiryLocked(Map::iterator it) {
  if (!it->second.expires_at.has_value()) return;
  expiry_.erase({*it->second.expires_at, it->first});
  it->second.expires_at.reset();
}

void Db::EraseLocked(Map::iterator it) {
  ClearExpiryLocked(it);
  map_.erase(it);
}

Db::Map::iterator Db::FindLive(std::string_view key) {
  auto it = map_.find(std::string(key));
  if (it == map_.end()) return it;
  const auto& expires_at = it->second.expires_at;
  if (expires_at.has_value() && now_() >= *expires_at) {
    EraseLocked(it);
    return map_.end();
  }
  return it;
}

}  // namespace qey
