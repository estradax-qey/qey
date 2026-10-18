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
#include "qey/posix_file.h"
#include "qey/status.h"

namespace qey {

Db::Db() : Db(&Clock::now) {}

Db::Db(NowFn now)
    : Db(std::move(now), [] { return std::chrono::system_clock::now(); }) {}

Db::Db(NowFn now, WallNowFn wall_now)
    : now_(std::move(now)), wall_now_(std::move(wall_now)) {}

StatusOr<std::unique_ptr<Db>> Db::Open(std::string_view path,
                                       OpenOptions options) {
  StatusOr<std::unique_ptr<PosixFile>> file =
      PosixFile::Open(std::string(path));
  if (!file.ok()) return file.status();
  return Open(std::unique_ptr<File>(*std::move(file)), std::move(options));
}

StatusOr<std::unique_ptr<Db>> Db::Open(std::unique_ptr<File> file,
                                       OpenOptions options) {
  if (file == nullptr) {
    return Status(StatusCode::kInvalidArgument, "file is null");
  }
  NowFn now = options.now ? std::move(options.now) : NowFn(&Clock::now);
  WallNowFn wall_now =
      options.wall_now
          ? std::move(options.wall_now)
          : WallNowFn([] { return std::chrono::system_clock::now(); });
  std::unique_ptr<Db> db(new Db(std::move(now), std::move(wall_now)));
  db->file_ = std::move(file);
  db->sync_ = options.sync;
  Status s = db->Recover(options.recovery);
  if (!s.ok()) return s;
  return StatusOr<std::unique_ptr<Db>>(std::move(db));
}

Status Db::Recover(Recovery recovery) {
  StatusOr<ReplayResult> replay = Replay(*file_, recovery, [this](Record&& r) {
    ApplyRecordLocked(std::move(r));
  });
  if (!replay.ok()) return replay.status();

  if (replay->valid_size == 0) {
    // A new file, or one whose header write was torn.
    if (file_->Size() > 0) {
      Status s = file_->Truncate(0);
      if (!s.ok()) return s;
    }
    Status s = WriteHeader(*file_);
    if (s.ok()) s = file_->Sync();
    if (!s.ok()) return s;
  } else if (replay->dropped_bytes > 0) {
    Status s = file_->Truncate(replay->valid_size);
    if (s.ok()) s = file_->Sync();
    if (!s.ok()) return s;
  }

  recovery_info_.records_replayed = replay->records;
  recovery_info_.dropped_bytes = replay->dropped_bytes;
  Sweep();  // Drop keys that expired while the store was closed.
  return Status::Ok();
}

Status Db::Set(std::string_view key, std::string_view value,
               std::optional<std::chrono::milliseconds> ttl) {
  std::lock_guard<std::mutex> lock(mu_);
  std::optional<Clock::time_point> expires_at;
  if (ttl.has_value()) expires_at = now_() + *ttl;

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
  auto it = FindLive(key);
  if (it == map_.end()) return false;
  Status s = LogLocked(Record{Record::Type::kDel, std::string(key), "", {}});
  if (!s.ok()) return s;
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
  const Clock::time_point at = now_() + ttl;
  Status s = LogLocked(
      Record{Record::Type::kExpire, std::string(key), "", ToWallMs(at)});
  if (!s.ok()) return s;
  SetExpiryLocked(it, at);
  return true;
}

StatusOr<bool> Db::Persist(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
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
  if (file_ == nullptr) return Status::Ok();
  if (failed_) {
    return Status(StatusCode::kIoError, "database failed after an I/O error");
  }
  Status s = file_->Sync();
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
  if (file_ == nullptr) return Status::Ok();
  if (failed_) {
    return Status(StatusCode::kIoError, "database failed after an I/O error");
  }

  const uint64_t size_before = file_->Size();
  Status s = AppendRecord(*file_, record);
  if (!s.ok()) {
    // Remove any partial record so that later appends do not follow garbage.
    if (!file_->Truncate(size_before).ok()) failed_ = true;
    return s;
  }
  if (sync_ == SyncPolicy::kAlways) {
    s = file_->Sync();
    if (!s.ok()) {
      // The record may or may not have reached the disk. Memory has not been
      // updated, so stop accepting writes rather than let the two diverge.
      failed_ = true;
      return s;
    }
  }
  return Status::Ok();
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
  const auto wall =
      wall_now_() +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(at -
                                                                      now_());
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         wall.time_since_epoch())
                         .count();
  return std::max<int64_t>(ms, 0);
}

Db::Clock::time_point Db::FromWallMs(int64_t unix_ms) {
  const auto at = std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::milliseconds(unix_ms)));
  return now_() + std::chrono::duration_cast<Clock::duration>(at - wall_now_());
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
