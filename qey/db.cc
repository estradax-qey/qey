#include "qey/db.h"

#include <chrono>
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
