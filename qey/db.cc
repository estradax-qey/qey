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

// Returns `*current + delta` (or minus) where `current` is null for a missing
// key, which counts as 0. Fails if the value is not an integer or the result
// does not fit.
StatusOr<int64_t> Added(const std::string* current_value, int64_t delta,
                        bool subtract) {
  int64_t current = 0;
  if (current_value != nullptr) {
    const std::string& s = *current_value;
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
  return result;
}

Status CheckKey(std::string_view key) {
  if (key.size() > Db::kMaxKeySize) {
    return Status(StatusCode::kInvalidArgument,
                  "key is longer than kMaxKeySize");
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
  if (options.sync == SyncPolicy::kInterval &&
      options.sync_interval <= std::chrono::milliseconds(0)) {
    return Status(StatusCode::kInvalidArgument,
                  "sync_interval must be positive");
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
  if (options.sync == SyncPolicy::kInterval && options.sync_thread) {
    Db* raw = db.get();
    db->sync_thread_ = std::thread([raw] { raw->SyncThreadMain(); });
  }
  return StatusOr<std::unique_ptr<Db>>(std::move(db));
}

Db::~Db() {
  if (sync_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    stop_cv_.notify_all();
    sync_thread_.join();
  }
  if (storage_ != nullptr) {
    // A clean close leaves nothing unsynced, whatever the policy.
    std::lock_guard<std::mutex> lock(mu_);
    if (unsynced_ && failure_.ok()) SyncLocked().IgnoreError();
  }
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

  last_sync_ = now_();
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
  Status valid_key = CheckKey(key);
  if (!valid_key.ok()) return valid_key;
  if (value.size() > kMaxValueSize) {
    return Status(StatusCode::kInvalidArgument,
                  "value is longer than kMaxValueSize");
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

  auto it = InsertLocked(key);
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
  Status valid = CheckKey(key);
  if (!valid.ok()) return valid;
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  return AddLocked(key, delta, /*subtract=*/false);
}

StatusOr<int64_t> Db::Decr(std::string_view key, int64_t delta) {
  Status valid = CheckKey(key);
  if (!valid.ok()) return valid;
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

StatusOr<size_t> Db::Append(std::string_view key, std::string_view suffix) {
  Status valid = CheckKey(key);
  if (!valid.ok()) return valid;
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  auto it = FindLive(key);
  const size_t old_size = it == map_.end() ? 0 : it->second.value.size();
  if (suffix.size() > kMaxValueSize ||
      old_size > kMaxValueSize - suffix.size()) {
    return Status(StatusCode::kInvalidArgument,
                  "value would be longer than kMaxValueSize");
  }

  if (it == map_.end()) {
    // A new key is logged as a plain SET: replay must not depend on whether
    // an earlier, expired value is still around.
    Status s = LogLocked(Record{Record::Type::kSet, std::string(key),
                                std::string(suffix), std::nullopt});
    if (!s.ok()) return s;
    InsertLocked(key)->second.value = suffix;
  } else {
    Status s = LogLocked(Record{Record::Type::kAppend, std::string(key),
                                std::string(suffix), std::nullopt});
    if (!s.ok()) return s;
    it->second.value.append(suffix);
  }
  return old_size + suffix.size();
}

StatusOr<bool> Db::Rename(std::string_view from, std::string_view to) {
  Status valid = CheckKey(to);
  if (!valid.ok()) return valid;
  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  auto it = FindLive(from);
  if (it == map_.end()) return false;
  if (from == to) return true;

  Status s = LogLocked(
      Record{Record::Type::kRename, std::string(from), std::string(to), {}});
  if (!s.ok()) return s;
  RenameLocked(it, to);
  return true;
}

Status Db::Write(const WriteBatch& batch) {
  if (batch.empty()) return Status::Ok();
  // Everything that can be checked without looking at the data comes first,
  // so that a bad batch is refused before anything is touched.
  for (const WriteBatch::Op& op : batch.ops_) {
    Status valid = CheckKey(op.key);
    if (valid.ok() && op.type == WriteBatch::Op::Type::kRename) {
      valid = CheckKey(op.arg);
    }
    if (valid.ok() && op.ttl.has_value()) valid = CheckTtl(*op.ttl);
    if (valid.ok() && op.arg.size() > kMaxValueSize &&
        op.type != WriteBatch::Op::Type::kRename) {
      valid = Status(StatusCode::kInvalidArgument,
                     "value is longer than kMaxValueSize");
    }
    if (!valid.ok()) return valid;
  }

  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  if (storage_ != nullptr) {
    Status healthy = CheckHealthyLocked();
    if (!healthy.ok()) return healthy;
  }

  // Apply to memory as we go, so each change sees the ones before it, and
  // keep what is needed to undo it. Nothing else can look at memory until the
  // lock is released.
  Undo undo;
  std::vector<Record> effective;
  for (const WriteBatch::Op& op : batch.ops_) {
    Status s = StageLocked(op, &undo, &effective);
    if (!s.ok()) {
      RollbackLocked(undo);
      return s;
    }
  }
  if (effective.empty()) return Status::Ok();  // every change was a no-op

  Record record;
  if (effective.size() == 1) {
    record = std::move(effective[0]);
  } else {
    record.type = Record::Type::kBatch;
    record.records = std::move(effective);
  }
  Status s = LogLocked(record);
  if (!s.ok()) {
    RollbackLocked(undo);
    return s;
  }
  return Status::Ok();
}

Status Db::StageLocked(const WriteBatch::Op& op, Undo* undo,
                       std::vector<Record>* effective) {
  using Type = WriteBatch::Op::Type;

  auto remember = [&](std::string_view key) {
    if (undo->find(key) != undo->end()) return;
    auto it = map_.find(key);
    undo->emplace(std::string(key), it == map_.end()
                                        ? std::nullopt
                                        : std::optional<Entry>(it->second));
  };
  // Applies through the same code as replay, so a live batch and a recovered
  // one cannot differ.
  auto apply = [&](Record record) {
    Record copy = record;
    ApplyRecordLocked(std::move(copy));
    effective->push_back(std::move(record));
  };

  switch (op.type) {
    case Type::kSet: {
      remember(op.key);
      Record record{Record::Type::kSet, op.key, op.arg, std::nullopt};
      if (op.ttl.has_value()) {
        record.expires_at_ms = ToWallMs(now_() + ClampTtl(*op.ttl));
      }
      apply(std::move(record));
      break;
    }
    case Type::kDel: {
      if (FindLive(op.key) == map_.end()) break;
      remember(op.key);
      apply(Record{Record::Type::kDel, op.key, "", {}});
      break;
    }
    case Type::kExpire: {
      if (FindLive(op.key) == map_.end()) break;
      remember(op.key);
      apply(Record{Record::Type::kExpire, op.key, "",
                   ToWallMs(now_() + ClampTtl(*op.ttl))});
      break;
    }
    case Type::kPersist: {
      auto it = FindLive(op.key);
      if (it == map_.end() || !it->second.expires_at.has_value()) break;
      remember(op.key);
      apply(Record{Record::Type::kPersist, op.key, "", {}});
      break;
    }
    case Type::kAppend: {
      auto it = FindLive(op.key);
      const size_t old_size = it == map_.end() ? 0 : it->second.value.size();
      if (old_size > kMaxValueSize - op.arg.size()) {
        return Status(StatusCode::kInvalidArgument,
                      "value would be longer than kMaxValueSize");
      }
      remember(op.key);
      // A new key is a plain SET, as in Append().
      apply(
          Record{it == map_.end() ? Record::Type::kSet : Record::Type::kAppend,
                 op.key, op.arg, std::nullopt});
      break;
    }
    case Type::kRename: {
      if (FindLive(op.key) == map_.end() || op.key == op.arg) break;
      FindLive(op.arg);  // drops an expired destination
      remember(op.key);
      remember(op.arg);
      apply(Record{Record::Type::kRename, op.key, op.arg, {}});
      break;
    }
    case Type::kIncr:
    case Type::kDecr: {
      auto it = FindLive(op.key);
      StatusOr<int64_t> added =
          Added(it == map_.end() ? nullptr : &it->second.value, op.delta,
                op.type == Type::kDecr);
      if (!added.ok()) return added.status();
      Record record{Record::Type::kSet, op.key, std::to_string(*added),
                    std::nullopt};
      if (it != map_.end() && it->second.expires_at.has_value()) {
        record.expires_at_ms = ToWallMs(*it->second.expires_at);
      }
      remember(op.key);
      apply(std::move(record));
      break;
    }
  }
  return Status::Ok();
}

void Db::RollbackLocked(const Undo& undo) {
  for (const auto& [key, original] : undo) {
    auto it = map_.find(key);
    if (!original.has_value()) {
      if (it != map_.end()) EraseLocked(it);
      continue;
    }
    if (it == map_.end()) it = InsertLocked(key);
    ClearExpiryLocked(it);
    it->second.value = original->value;
    if (original->expires_at.has_value()) {
      SetExpiryLocked(it, *original->expires_at);
    }
  }
}

std::vector<std::optional<std::string>> Db::MGet(
    const std::vector<std::string>& keys) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::optional<std::string>> values;
  values.reserve(keys.size());
  for (const std::string& key : keys) {
    auto it = FindLive(key);
    if (it == map_.end()) {
      values.emplace_back(std::nullopt);
    } else {
      values.emplace_back(it->second.value);
    }
  }
  return values;
}

Status Db::Clear() {
  std::lock_guard<std::mutex> lock(mu_);
  if (storage_ != nullptr) {
    Status healthy = CheckHealthyLocked();
    if (!healthy.ok()) return healthy;
    Status s = CompactLocked(/*empty=*/true);
    if (!s.ok()) return s;
  }
  keys_.clear();  // stays built: an empty index of an empty map
  map_.clear();
  expiry_.clear();
  return Status::Ok();
}

std::vector<std::string> Db::Keys(std::string_view prefix) {
  return Scan(prefix, std::numeric_limits<size_t>::max()).keys;
}

ScanPage Db::Scan(std::string_view prefix, size_t limit,
                  const std::optional<std::string>& after) {
  std::lock_guard<std::mutex> lock(mu_);
  EnsureKeyIndexLocked();
  limit = std::max<size_t>(limit, 1);
  const Clock::time_point now = now_();

  auto it = (after.has_value() && std::string_view(*after) >= prefix)
                ? keys_.upper_bound(std::string_view(*after))
                : keys_.lower_bound(prefix);
  ScanPage page;
  // Keys are ordered, so everything with the prefix is contiguous.
  for (; it != keys_.end() && it->starts_with(prefix); ++it) {
    auto entry = map_.find(*it);
    assert(entry != map_.end());
    const auto& expires_at = entry->second.expires_at;
    if (expires_at.has_value() && now >= *expires_at) continue;
    if (page.keys.size() == limit) {
      page.next = page.keys.back();  // there is at least one more
      break;
    }
    page.keys.emplace_back(*it);
  }
  return page;
}

Status Db::Sync() {
  std::lock_guard<std::mutex> lock(mu_);
  if (storage_ == nullptr) return Status::Ok();
  Status healthy = CheckHealthyLocked();
  if (!healthy.ok()) return healthy;
  return SyncLocked();
}

Status Db::SyncIfDue() {
  std::lock_guard<std::mutex> lock(mu_);
  if (storage_ == nullptr || options_.sync != SyncPolicy::kInterval) {
    return Status::Ok();
  }
  Status healthy = CheckHealthyLocked();
  if (!healthy.ok()) return healthy;
  if (!unsynced_ || now_() - last_sync_ < options_.sync_interval) {
    return Status::Ok();
  }
  return SyncLocked();
}

Status Db::CheckHealthyLocked() const {
  if (failure_.ok()) return Status::Ok();
  return Status(StatusCode::kIoError, "database failed after an I/O error (" +
                                          failure_.message() + "); reopen it");
}

void Db::FailLocked(const Status& cause) {
  if (failure_.ok()) failure_ = cause;
}

Status Db::SyncLocked() {
  Status s = storage_->log().Sync();
  if (!s.ok()) {
    FailLocked(s);
    return s;
  }
  unsynced_ = false;
  last_sync_ = now_();
  return Status::Ok();
}

void Db::SyncThreadMain() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!stopping_) {
    stop_cv_.wait_for(lock, options_.sync_interval,
                      [this] { return stopping_; });
    if (stopping_) break;
    if (unsynced_ && failure_.ok()) SyncLocked().IgnoreError();
  }
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
  StatusOr<int64_t> added =
      Added(it == map_.end() ? nullptr : &it->second.value, delta, subtract);
  if (!added.ok()) return added.status();
  const int64_t result = *added;

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
    InsertLocked(key)->second.value = std::move(text);
  }
  return result;
}

Status Db::LogLocked(const Record& record) {
  if (storage_ == nullptr) return Status::Ok();
  Status healthy = CheckHealthyLocked();
  if (!healthy.ok()) return healthy;

  File& log = storage_->log();
  const uint64_t size_before = log.Size();
  Status s = AppendRecord(log, record);
  if (!s.ok()) {
    // Remove any partial record so that later appends do not follow garbage.
    Status undone = log.Truncate(size_before);
    if (!undone.ok()) FailLocked(undone);
    return s;
  }
  unsynced_ = true;
  const bool sync_now = options_.sync == SyncPolicy::kAlways ||
                        (options_.sync == SyncPolicy::kInterval &&
                         now_() - last_sync_ >= options_.sync_interval);
  if (sync_now) {
    // If this fails, the record may or may not have reached the disk. Memory
    // has not been updated, so SyncLocked() stops further writes rather than
    // let the two diverge.
    return SyncLocked();
  }
  return Status::Ok();
}

Status Db::Compact() {
  std::lock_guard<std::mutex> lock(mu_);
  if (storage_ == nullptr) return Status::Ok();
  Status healthy = CheckHealthyLocked();
  if (!healthy.ok()) return healthy;
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
  if (storage_ == nullptr || !failure_.ok() || !options_.auto_compact) return;
  const uint64_t size = storage_->log().Size();
  if (size <= compact_threshold_) return;
  if (!CompactLocked().ok()) {
    // Do not retry on every write; try again after the log has grown more.
    compact_threshold_ = size + options_.compact_min_bytes;
  }
}

Status Db::CompactLocked(bool empty) {
  const Clock::time_point now = now_();
  Status s = storage_->Replace([&](File& out) -> Status {
    // Batch records so that a big database is not written one call per key.
    constexpr size_t kFlushBytes = 1 << 20;
    std::string buf = EncodeHeader();
    for (const auto& [key, entry] : map_) {
      if (empty) break;
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
  // Replace() synced the new log, so everything so far is on disk.
  unsynced_ = false;
  last_sync_ = now_();
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
      auto it = InsertLocked(record.key);
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
    case Record::Type::kAppend: {
      // The key was live when this was logged, so it exists unless the log
      // was edited; tolerate that by creating it.
      auto it = InsertLocked(record.key);
      it->second.value.append(record.value);
      break;
    }
    case Record::Type::kRename: {
      auto it = map_.find(record.key);
      if (it != map_.end() && record.key != record.value) {
        RenameLocked(it, record.value);
      }
      break;
    }
    case Record::Type::kBatch:
      for (Record& member : record.records) {
        ApplyRecordLocked(std::move(member));
      }
      break;
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

void Db::RenameLocked(Map::iterator it, std::string_view to) {
  // Whatever is at `to`, live or expired, is replaced.
  auto dest = map_.find(to);
  if (dest != map_.end()) EraseLocked(dest);

  const std::optional<Clock::time_point> expires_at = it->second.expires_at;
  ClearExpiryLocked(it);
  if (keys_built_)
    keys_.erase(std::string_view(it->first));  // before it changes
  auto node = map_.extract(it);
  node.key() = std::string(to);
  auto inserted = map_.insert(std::move(node));
  if (keys_built_) keys_.insert(std::string_view(inserted.position->first));
  if (expires_at.has_value()) SetExpiryLocked(inserted.position, *expires_at);
}

Db::Map::iterator Db::InsertLocked(std::string_view key) {
  auto it = map_.find(key);
  if (it != map_.end()) return it;
  it = map_.try_emplace(std::string(key)).first;
  if (keys_built_) keys_.insert(std::string_view(it->first));
  return it;
}

void Db::EnsureKeyIndexLocked() {
  if (keys_built_) return;
  // Sorting first lets the set be filled with cheap end-of-set hints.
  std::vector<std::string_view> sorted;
  sorted.reserve(map_.size());
  for (const auto& entry : map_) sorted.emplace_back(entry.first);
  std::sort(sorted.begin(), sorted.end());
  for (std::string_view key : sorted) keys_.emplace_hint(keys_.end(), key);
  keys_built_ = true;
}

void Db::EraseLocked(Map::iterator it) {
  ClearExpiryLocked(it);
  if (keys_built_) keys_.erase(std::string_view(it->first));  // before it dies
  map_.erase(it);
}

Db::Map::iterator Db::FindLive(std::string_view key) {
  auto it = map_.find(key);
  if (it == map_.end()) return it;
  const auto& expires_at = it->second.expires_at;
  if (expires_at.has_value() && now_() >= *expires_at) {
    EraseLocked(it);
    return map_.end();
  }
  return it;
}

}  // namespace qey
