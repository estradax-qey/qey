#include "qey/db.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "qey/file.h"
#include "qey/log.h"
#include "qey/posix_storage.h"
#include "qey/status.h"
#include "qey/storage.h"

namespace qey {
namespace {

Status CheckTtl(std::chrono::milliseconds ttl) {
  if (ttl > Db::kMaxTtl) {
    return Status(StatusCode::kInvalidArgument, "ttl is longer than kMaxTtl");
  }
  return Status::Ok();
}

// A TTL that is not positive expires the key immediately. Clamping it to zero
// also keeps very negative values from overflowing time arithmetic.
std::chrono::milliseconds ClampTtl(std::chrono::milliseconds ttl) {
  return std::max(ttl, std::chrono::milliseconds(0));
}

}  // namespace

Db::Db() : Db(&Clock::now) {}

Db::Db(NowFn now)
    : Db(std::move(now), [] { return std::chrono::system_clock::now(); }) {}

Db::Db(NowFn now, WallNowFn wall_now)
    : now_(std::move(now)), wall_now_(std::move(wall_now)) {}

StatusOr<std::unique_ptr<Db>> Db::Open(std::string_view path,
                                       OpenOptions options) {
  StatusOr<std::unique_ptr<PosixStorage>> storage =
      PosixStorage::Open(std::string(path));
  if (!storage.ok()) return storage.status();
  return Open(std::unique_ptr<Storage>(*std::move(storage)),
              std::move(options));
}

StatusOr<std::unique_ptr<Db>> Db::Open(std::unique_ptr<Storage> storage,
                                       OpenOptions options) {
  if (storage == nullptr) {
    return Status(StatusCode::kInvalidArgument, "storage is null");
  }
  options.compact_ratio = std::max<uint32_t>(options.compact_ratio, 1);
  NowFn now = options.now ? std::move(options.now) : NowFn(&Clock::now);
  WallNowFn wall_now =
      options.wall_now
          ? std::move(options.wall_now)
          : WallNowFn([] { return std::chrono::system_clock::now(); });
  std::unique_ptr<Db> db(new Db(std::move(now), std::move(wall_now)));
  db->storage_ = std::move(storage);
  db->options_ = options;
  Status s = db->Recover(options.recovery);
  if (!s.ok()) return s;
  return StatusOr<std::unique_ptr<Db>>(std::move(db));
}

Status Db::Recover(Recovery recovery) {
  StatusOr<ReplayResult> replay =
      Replay(storage_->log(), recovery,
             [this](Record&& r) { ApplyRecordLocked(std::move(r)); });
  if (!replay.ok()) return replay.status();

  if (replay->valid_size == 0) {
    // A new file, or one whose header write was torn.
    File& log = storage_->log();
    if (log.Size() > 0) {
      Status s = log.Truncate(0);
      if (!s.ok()) return s;
    }
    Status s = WriteHeader(log);
    if (s.ok()) s = log.Sync();
    if (!s.ok()) return s;
  } else if (replay->dropped_bytes > 0) {
    File& log = storage_->log();
    Status s = log.Truncate(replay->valid_size);
    if (s.ok()) s = log.Sync();
    if (!s.ok()) return s;
  }

  recovery_info_.records_replayed = replay->records;
  recovery_info_.dropped_bytes = replay->dropped_bytes;
  Sweep();  // Drop keys that expired while the store was closed.

  // A log much bigger than the data in it is compacted right away.
  uint64_t live_bytes = kLogHeaderSize;
  for (const auto& [key, entry] : map_) {
    live_bytes += 8 + 1 + 4 + key.size() + 4 + entry.value.size() + 8;
  }
  SetCompactThreshold(live_bytes);
  MaybeCompactLocked();
  return Status::Ok();
}

Status Db::Set(std::string_view key, std::string_view value,
               std::optional<std::chrono::milliseconds> ttl) {
  if (ttl.has_value()) {
    Status valid = CheckTtl(*ttl);
    if (!valid.ok()) return valid;
  }
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  std::optional<Clock::time_point> expires_at;
  if (ttl.has_value()) expires_at = now_() + ClampTtl(*ttl);

  Record record{Record::Type::kSet, std::string(key), std::string(value),
                std::nullopt};
  if (expires_at.has_value()) record.expires_at_ms = ToWallMs(*expires_at);
  Status s = LogLocked(record);
  if (!s.ok()) return s;

  auto it = map_.try_emplace(std::string(key)).first;
  ClearExpiryLocked(it);
  it->second.value = std::string(value);
  if (expires_at.has_value()) SetExpiryLocked(it, *expires_at);
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
  MaybeCompactLocked();
  auto it = FindLive(key);
  if (it == map_.end()) return false;
  Status s = LogLocked(Record{Record::Type::kDel, std::string(key), "", {}});
  if (!s.ok()) return s;
  EraseLocked(it);
  return true;
}

StatusOr<int64_t> Db::Incr(std::string_view key, int64_t delta) {
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  return AddLocked(key, delta, /*subtract=*/false);
}

StatusOr<int64_t> Db::Decr(std::string_view key, int64_t delta) {
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
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
  Status valid = CheckTtl(ttl);
  if (!valid.ok()) return valid;
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  auto it = FindLive(key);
  if (it == map_.end()) return false;
  const Clock::time_point at = now_() + ClampTtl(ttl);
  Status s = LogLocked(
      Record{Record::Type::kExpire, std::string(key), "", ToWallMs(at)});
  if (!s.ok()) return s;
  SetExpiryLocked(it, at);
  return true;
}

StatusOr<bool> Db::Persist(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  auto it = FindLive(key);
  if (it == map_.end() || !it->second.expires_at.has_value()) return false;
  Status s =
      LogLocked(Record{Record::Type::kPersist, std::string(key), "", {}});
  if (!s.ok()) return s;
  ClearExpiryLocked(it);
  return true;
}

Status Db::Sync() {
  std::lock_guard<std::mutex> lock(mu_);
  if (storage_ == nullptr) return Status::Ok();
  if (failed_) {
    return Status(StatusCode::kIoError, "database failed after an I/O error");
  }
  Status s = storage_->log().Sync();
  if (!s.ok()) failed_ = true;
  return s;
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
  // Log the resulting value, not the delta, so replay never redoes arithmetic.
  Record record{Record::Type::kSet, std::string(key), text, std::nullopt};
  if (it != map_.end() && it->second.expires_at.has_value()) {
    record.expires_at_ms = ToWallMs(*it->second.expires_at);
  }
  Status s = LogLocked(record);
  if (!s.ok()) return s;

  if (it != map_.end()) {
    it->second.value = std::move(text);
  } else {
    map_.insert_or_assign(std::string(key),
                          Entry{std::move(text), std::nullopt});
  }
  return result;
}

Status Db::LogLocked(const Record& record) {
  if (storage_ == nullptr) return Status::Ok();
  if (failed_) {
    return Status(StatusCode::kIoError, "database failed after an I/O error");
  }

  File& log = storage_->log();
  const uint64_t size_before = log.Size();
  Status s = AppendRecord(log, record);
  if (!s.ok()) {
    // Remove any partial record so that later appends do not follow garbage.
    if (!log.Truncate(size_before).ok()) failed_ = true;
    return s;
  }
  if (options_.sync == SyncPolicy::kAlways) {
    s = log.Sync();
    if (!s.ok()) {
      // The record may or may not have reached the disk. Memory has not been
      // updated, so stop accepting writes rather than let the two diverge.
      failed_ = true;
      return s;
    }
  }
  return Status::Ok();
}

Status Db::Compact() {
  std::lock_guard<std::mutex> lock(mu_);
  if (storage_ == nullptr) return Status::Ok();
  if (failed_) {
    return Status(StatusCode::kIoError, "database failed after an I/O error");
  }
  Status s = CompactLocked();
  if (s.ok()) {
    // Nothing is iterating the map here, so expired keys can go.
    const Clock::time_point now = now_();
    while (!expiry_.empty() && expiry_.begin()->first <= now) {
      EraseLocked(map_.find(expiry_.begin()->second));
    }
  }
  return s;
}

void Db::MaybeCompactLocked() {
  if (storage_ == nullptr || failed_ || !options_.auto_compact) return;
  const uint64_t size = storage_->log().Size();
  if (size <= compact_threshold_) return;
  if (!CompactLocked().ok()) {
    // Do not retry on every write; try again after the log has grown more.
    compact_threshold_ = size + options_.compact_min_bytes;
  }
}

Status Db::CompactLocked() {
  const Clock::time_point now = now_();
  Status s = storage_->Replace([&](File& out) -> Status {
    // Batch records so that a big database is not written one call per key.
    constexpr size_t kFlushBytes = 1 << 20;
    std::string buf = EncodeHeader();
    for (const auto& [key, entry] : map_) {
      if (entry.expires_at.has_value() && *entry.expires_at <= now) continue;
      Record record{Record::Type::kSet, key, entry.value, std::nullopt};
      if (entry.expires_at.has_value()) {
        record.expires_at_ms = ToWallMs(*entry.expires_at);
      }
      Status encoded = EncodeRecord(record, &buf);
      if (!encoded.ok()) return encoded;
      if (buf.size() >= kFlushBytes) {
        Status written = out.Append(buf);
        if (!written.ok()) return written;
        buf.clear();
      }
    }
    return out.Append(buf);
  });
  if (!s.ok()) return s;
  SetCompactThreshold(storage_->log().Size());
  return Status::Ok();
}

void Db::SetCompactThreshold(uint64_t base_size) {
  compact_threshold_ = std::max<uint64_t>(options_.compact_min_bytes,
                                          base_size * options_.compact_ratio);
}

void Db::ApplyRecordLocked(Record&& record) {
  switch (record.type) {
    case Record::Type::kSet: {
      auto it = map_.try_emplace(std::move(record.key)).first;
      ClearExpiryLocked(it);
      it->second.value = std::move(record.value);
      if (record.expires_at_ms.has_value()) {
        SetExpiryLocked(it, FromWallMs(*record.expires_at_ms));
      }
      break;
    }
    case Record::Type::kDel: {
      auto it = map_.find(record.key);
      if (it != map_.end()) EraseLocked(it);
      break;
    }
    case Record::Type::kExpire: {
      auto it = map_.find(record.key);
      if (it != map_.end()) {
        SetExpiryLocked(it, FromWallMs(*record.expires_at_ms));
      }
      break;
    }
    case Record::Type::kPersist: {
      auto it = map_.find(record.key);
      if (it != map_.end()) ClearExpiryLocked(it);
      break;
    }
  }
}

int64_t Db::ToWallMs(Clock::time_point at) {
  // Work in whole milliseconds. `at` is at most kMaxTtl from now, so none of
  // this can overflow.
  const int64_t wall_now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          wall_now_().time_since_epoch())
          .count();
  const int64_t delta_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(at - now_())
          .count();
  return std::max<int64_t>(wall_now_ms + delta_ms, 0);
}

Db::Clock::time_point Db::FromWallMs(int64_t unix_ms) {
  // The log may hold any value, so saturate instead of overflowing: an expiry
  // further away than kMaxTtl is treated as kMaxTtl away.
  const int64_t wall_now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          wall_now_().time_since_epoch())
          .count();
  const int64_t max_ms = kMaxTtl.count();
  int64_t delta_ms;
  if (__builtin_sub_overflow(unix_ms, wall_now_ms, &delta_ms)) {
    delta_ms = max_ms;
  }
  delta_ms = std::clamp<int64_t>(delta_ms, -max_ms, max_ms);
  return now_() + std::chrono::milliseconds(delta_ms);
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
