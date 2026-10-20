#include "qey/db.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
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

// Resolves a possibly negative list index against a list of `size` elements.
// Returns false if it falls outside the list.
bool ResolveIndex(int64_t index, size_t size, size_t* resolved) {
  const int64_t n = static_cast<int64_t>(size);
  if (index < 0) index += n;
  if (index < 0 || index >= n) return false;
  *resolved = static_cast<size_t>(index);
  return true;
}

// Resolves an inclusive range with possibly negative ends against a list of
// `size` elements, clamping to what exists. Returns false if the range holds
// no elements.
bool ResolveRange(int64_t start, int64_t stop, size_t size, size_t* first,
                  size_t* last) {
  const int64_t n = static_cast<int64_t>(size);
  if (start < 0) start = std::max<int64_t>(n + start, 0);
  if (stop < 0) stop = n + stop;
  if (stop >= n) stop = n - 1;
  if (start > stop || start >= n) return false;
  *first = static_cast<size_t>(start);
  *last = static_cast<size_t>(stop);
  return true;
}

Status WrongType() {
  return Status(StatusCode::kWrongType,
                "the key holds a different type of value");
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
  // Every expiry in the log is placed relative to this one instant.
  const ClockPoint ref = ReadClocks();
  StatusOr<ReplayResult> replay = Replay(
      storage_->log(), recovery,
      [this, &ref](Record&& r) { ApplyRecordLocked(std::move(r), ref); });
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
    live_bytes +=
        8 + 1 + 4 + key.size() + 4 + entry.value.ApproxEncodedBytes() + 8;
  }
  SetCompactThreshold(live_bytes);
  MaybeCompactLocked();
  return Status::Ok();
}

Status Db::Set(std::string_view key, std::string_view value,
               std::optional<std::chrono::milliseconds> ttl) {
  WriteBatch::Op op =
      WriteBatch::MakeOp(WriteBatch::Op::Type::kSet, key, value);
  op.ttl = ttl;
  return Run(op).status();
}

std::optional<std::string> Db::Get(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  if (it == map_.end() || it->second.value.type() != KeyType::kString) {
    return std::nullopt;
  }
  return it->second.value.str();
}

KeyType Db::Type(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = FindLive(key);
  return it == map_.end() ? KeyType::kNone : it->second.value.type();
}

StatusOr<bool> Db::Del(std::string_view key) {
  StatusOr<OpResult> r =
      Run(WriteBatch::MakeOp(WriteBatch::Op::Type::kDel, key));
  if (!r.ok()) return r.status();
  return r->flag;
}

StatusOr<int64_t> Db::Incr(std::string_view key, int64_t delta) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kIncr, key);
  op.n1 = delta;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->integer;
}

StatusOr<int64_t> Db::Decr(std::string_view key, int64_t delta) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kDecr, key);
  op.n1 = delta;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->integer;
}

StatusOr<size_t> Db::HSet(std::string_view key, std::string_view field,
                          std::string_view value) {
  return HSet(
      key, std::vector<FieldValue>{{std::string(field), std::string(value)}});
}

StatusOr<size_t> Db::HSet(std::string_view key,
                          const std::vector<FieldValue>& fields) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kHSet, key);
  op.items.reserve(fields.size() * 2);
  for (const FieldValue& fv : fields) {
    op.items.push_back(fv.first);
    op.items.push_back(fv.second);
  }
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<std::optional<std::string>> Db::HGet(std::string_view key,
                                              std::string_view field) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kHash, &it);
  if (!s.ok()) return s;
  if (it == map_.end()) return std::optional<std::string>();
  const Hash& hash = it->second.value.hash();
  auto f = hash.find(field);
  if (f == hash.end()) return std::optional<std::string>();
  return std::optional<std::string>(f->second);
}

StatusOr<size_t> Db::HDel(std::string_view key,
                          const std::vector<std::string>& fields) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kHDel, key);
  op.items = fields;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<bool> Db::HExists(std::string_view key, std::string_view field) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kHash, &it);
  if (!s.ok()) return s;
  if (it == map_.end()) return false;
  const Hash& hash = it->second.value.hash();
  return hash.find(field) != hash.end();
}

StatusOr<size_t> Db::HLen(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kHash, &it);
  if (!s.ok()) return s;
  return it == map_.end() ? size_t{0} : it->second.value.hash().size();
}

StatusOr<std::vector<FieldValue>> Db::HGetAll(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kHash, &it);
  if (!s.ok()) return s;
  std::vector<FieldValue> all;
  if (it == map_.end()) return all;
  all.reserve(it->second.value.hash().size());
  for (const auto& [field, value] : it->second.value.hash()) {
    all.emplace_back(field, value);
  }
  return all;
}

StatusOr<int64_t> Db::HIncr(std::string_view key, std::string_view field,
                            int64_t delta) {
  WriteBatch::Op op =
      WriteBatch::MakeOp(WriteBatch::Op::Type::kHIncr, key, field);
  op.n1 = delta;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->integer;
}

StatusOr<size_t> Db::LPush(std::string_view key,
                           const std::vector<std::string>& values) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kLPush, key);
  op.items = values;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<size_t> Db::RPush(std::string_view key,
                           const std::vector<std::string>& values) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kRPush, key);
  op.items = values;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<std::vector<std::string>> Db::LPop(std::string_view key,
                                            size_t count) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kLPop, key);
  op.n1 = static_cast<int64_t>(std::min<size_t>(count, INT64_MAX));
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return std::move(r->items);
}

StatusOr<std::vector<std::string>> Db::RPop(std::string_view key,
                                            size_t count) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kRPop, key);
  op.n1 = static_cast<int64_t>(std::min<size_t>(count, INT64_MAX));
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return std::move(r->items);
}

StatusOr<size_t> Db::LLen(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kList, &it);
  if (!s.ok()) return s;
  return it == map_.end() ? size_t{0} : it->second.value.list().size();
}

StatusOr<std::optional<std::string>> Db::LIndex(std::string_view key,
                                                int64_t index) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kList, &it);
  if (!s.ok()) return s;
  size_t resolved;
  if (it == map_.end() ||
      !ResolveIndex(index, it->second.value.list().size(), &resolved)) {
    return std::optional<std::string>();
  }
  return std::optional<std::string>(it->second.value.list()[resolved]);
}

StatusOr<std::vector<std::string>> Db::LRange(std::string_view key,
                                              int64_t start, int64_t stop) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kList, &it);
  if (!s.ok()) return s;
  std::vector<std::string> out;
  size_t first;
  size_t last;
  if (it == map_.end() ||
      !ResolveRange(start, stop, it->second.value.list().size(), &first,
                    &last)) {
    return out;
  }
  const List& list = it->second.value.list();
  out.assign(list.begin() + static_cast<std::ptrdiff_t>(first),
             list.begin() + static_cast<std::ptrdiff_t>(last) + 1);
  return out;
}

StatusOr<bool> Db::LSet(std::string_view key, int64_t index,
                        std::string_view value) {
  WriteBatch::Op op =
      WriteBatch::MakeOp(WriteBatch::Op::Type::kLSet, key, value);
  op.n1 = index;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->flag;
}

Status Db::LTrim(std::string_view key, int64_t start, int64_t stop) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kLTrim, key);
  op.n1 = start;
  op.n2 = stop;
  return Run(op).status();
}

StatusOr<size_t> Db::SAdd(std::string_view key,
                          const std::vector<std::string>& members) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kSAdd, key);
  op.items = members;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<size_t> Db::SRem(std::string_view key,
                          const std::vector<std::string>& members) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kSRem, key);
  op.items = members;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<bool> Db::SIsMember(std::string_view key, std::string_view member) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kSet, &it);
  if (!s.ok()) return s;
  if (it == map_.end()) return false;
  const StringSet& set = it->second.value.set();
  return set.find(member) != set.end();
}

StatusOr<size_t> Db::SCard(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kSet, &it);
  if (!s.ok()) return s;
  return it == map_.end() ? size_t{0} : it->second.value.set().size();
}

StatusOr<std::vector<std::string>> Db::SMembers(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kSet, &it);
  if (!s.ok()) return s;
  std::vector<std::string> members;
  if (it != map_.end()) {
    const StringSet& set = it->second.value.set();
    members.assign(set.begin(), set.end());
  }
  return members;
}

StatusOr<std::vector<std::string>> Db::SUnion(
    const std::vector<std::string>& keys) {
  return CombineSets(keys, 'u');
}

StatusOr<std::vector<std::string>> Db::SInter(
    const std::vector<std::string>& keys) {
  return CombineSets(keys, 'i');
}

StatusOr<std::vector<std::string>> Db::SDiff(
    const std::vector<std::string>& keys) {
  return CombineSets(keys, 'd');
}

StatusOr<std::vector<std::string>> Db::CombineSets(
    const std::vector<std::string>& keys, char op) {
  std::lock_guard<std::mutex> lock(mu_);
  // Look at every key first, so that a key of the wrong type fails the call
  // whatever the others hold.
  std::vector<const StringSet*> sets;
  sets.reserve(keys.size());
  static const StringSet kEmpty;
  for (const std::string& key : keys) {
    Map::iterator it;
    Status s = FindOfType(key, KeyType::kSet, &it);
    if (!s.ok()) return s;
    sets.push_back(it == map_.end() ? &kEmpty : &it->second.value.set());
  }
  StringSet result;
  if (!sets.empty()) {
    if (op == 'u') {
      for (const StringSet* set : sets) result.insert(set->begin(), set->end());
    } else {
      for (const std::string& member : *sets[0]) {
        bool keep = true;
        for (size_t i = 1; i < sets.size() && keep; ++i) {
          const bool in_other = sets[i]->find(member) != sets[i]->end();
          keep = op == 'i' ? in_other : !in_other;
        }
        if (keep) result.insert(member);
      }
    }
  }
  return std::vector<std::string>(result.begin(), result.end());
}

StatusOr<size_t> Db::ZAdd(std::string_view key,
                          const std::vector<ScoredMember>& members) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kZAdd, key);
  for (const ScoredMember& m : members) {
    op.items.push_back(m.member);
    op.scores.push_back(m.score);
  }
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<size_t> Db::ZRem(std::string_view key,
                          const std::vector<std::string>& members) {
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kZRem, key);
  op.items = members;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<std::optional<double>> Db::ZScore(std::string_view key,
                                           std::string_view member) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kZSet, &it);
  if (!s.ok()) return s;
  if (it == map_.end()) return std::optional<double>();
  const auto& scores = it->second.value.zset().scores;
  auto m = scores.find(member);
  if (m == scores.end()) return std::optional<double>();
  return std::optional<double>(m->second);
}

StatusOr<std::optional<size_t>> Db::ZRank(std::string_view key,
                                          std::string_view member) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kZSet, &it);
  if (!s.ok()) return s;
  if (it == map_.end()) return std::optional<size_t>();
  const ZSet& zset = it->second.value.zset();
  auto m = zset.scores.find(member);
  if (m == zset.scores.end()) return std::optional<size_t>();
  auto position = zset.order.find({m->second, m->first});
  return std::optional<size_t>(
      static_cast<size_t>(std::distance(zset.order.begin(), position)));
}

StatusOr<size_t> Db::ZCard(std::string_view key) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kZSet, &it);
  if (!s.ok()) return s;
  return it == map_.end() ? size_t{0} : it->second.value.zset().size();
}

StatusOr<std::vector<ScoredMember>> Db::ZRange(std::string_view key,
                                               int64_t start, int64_t stop) {
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kZSet, &it);
  if (!s.ok()) return s;
  std::vector<ScoredMember> out;
  size_t first;
  size_t last;
  if (it == map_.end() ||
      !ResolveRange(start, stop, it->second.value.zset().size(), &first,
                    &last)) {
    return out;
  }
  const ZSet& zset = it->second.value.zset();
  auto position = zset.order.begin();
  std::advance(position, static_cast<std::ptrdiff_t>(first));
  for (size_t i = first; i <= last; ++i, ++position) {
    out.push_back({position->second, position->first});
  }
  return out;
}

StatusOr<std::vector<ScoredMember>> Db::ZRangeByScore(std::string_view key,
                                                      double min, double max) {
  if (std::isnan(min) || std::isnan(max)) {
    return Status(StatusCode::kInvalidArgument, "a score bound cannot be NaN");
  }
  std::lock_guard<std::mutex> lock(mu_);
  Map::iterator it;
  Status s = FindOfType(key, KeyType::kZSet, &it);
  if (!s.ok()) return s;
  std::vector<ScoredMember> out;
  if (it == map_.end() || min > max) return out;
  const ZSet& zset = it->second.value.zset();
  for (auto position = zset.order.lower_bound({min, std::string()});
       position != zset.order.end() && position->first <= max; ++position) {
    out.push_back({position->second, position->first});
  }
  return out;
}

StatusOr<double> Db::ZIncr(std::string_view key, std::string_view member,
                           double delta) {
  WriteBatch::Op op =
      WriteBatch::MakeOp(WriteBatch::Op::Type::kZIncr, key, member);
  op.d = delta;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->number;
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
  WriteBatch::Op op = WriteBatch::MakeOp(WriteBatch::Op::Type::kExpire, key);
  op.ttl = ttl;
  StatusOr<OpResult> r = Run(op);
  if (!r.ok()) return r.status();
  return r->flag;
}

StatusOr<bool> Db::Persist(std::string_view key) {
  StatusOr<OpResult> r =
      Run(WriteBatch::MakeOp(WriteBatch::Op::Type::kPersist, key));
  if (!r.ok()) return r.status();
  return r->flag;
}

StatusOr<size_t> Db::Append(std::string_view key, std::string_view suffix) {
  StatusOr<OpResult> r =
      Run(WriteBatch::MakeOp(WriteBatch::Op::Type::kAppend, key, suffix));
  if (!r.ok()) return r.status();
  return r->count;
}

StatusOr<bool> Db::Rename(std::string_view from, std::string_view to) {
  StatusOr<OpResult> r =
      Run(WriteBatch::MakeOp(WriteBatch::Op::Type::kRename, from, to));
  if (!r.ok()) return r.status();
  return r->flag;
}

Status Db::ValidateOp(const WriteBatch::Op& op) {
  Status valid = CheckKey(op.key);
  if (!valid.ok()) return valid;
  if (op.type == WriteBatch::Op::Type::kRename) {
    valid = CheckKey(op.arg);
    if (!valid.ok()) return valid;
  } else if (op.arg.size() > kMaxValueSize) {
    return Status(StatusCode::kInvalidArgument,
                  "value is longer than kMaxValueSize");
  }
  for (const std::string& item : op.items) {
    if (item.size() > kMaxValueSize) {
      return Status(StatusCode::kInvalidArgument,
                    "an element is longer than kMaxValueSize");
    }
  }
  for (double score : op.scores) {
    if (std::isnan(score)) {
      return Status(StatusCode::kInvalidArgument, "a score cannot be NaN");
    }
  }
  if (std::isnan(op.d)) {
    return Status(StatusCode::kInvalidArgument, "a delta cannot be NaN");
  }
  if (op.type == WriteBatch::Op::Type::kHSet && op.items.size() % 2 != 0) {
    return Status(StatusCode::kInvalidArgument,
                  "hash fields and values come in pairs");
  }
  if (op.ttl.has_value()) return CheckTtl(*op.ttl);
  return Status::Ok();
}

StatusOr<Db::OpResult> Db::Run(const WriteBatch::Op& op) {
  Status valid = ValidateOp(op);
  if (!valid.ok()) return valid;

  std::lock_guard<std::mutex> lock(mu_);
  MaybeCompactLocked();
  const ClockPoint ref = ReadClocks();
  std::optional<Record> record;
  OpResult result;
  Status s = PlanLocked(op, ref, &record, &result);
  if (!s.ok()) return s;
  if (!record.has_value()) return result;  // it changes nothing

  // Log first, so a failed write leaves memory as it was; then apply through
  // the same code that replays the log.
  s = LogLocked(*record);
  if (!s.ok()) return s;
  ApplyRecordLocked(std::move(*record), ref);
  return result;
}

Status Db::PlanLocked(const WriteBatch::Op& op, const ClockPoint& ref,
                      std::optional<Record>* record, OpResult* result) {
  using Type = WriteBatch::Op::Type;
  switch (op.type) {
    case Type::kSet: {
      Record r{Record::Type::kSet, op.key, op.arg, std::nullopt};
      if (op.ttl.has_value()) {
        r.expires_at_ms = ToWallMs(ref.steady + ClampTtl(*op.ttl), ref);
      }
      *record = std::move(r);
      break;
    }
    case Type::kDel: {
      if (FindLive(op.key) == map_.end()) break;
      result->flag = true;
      *record = Record{Record::Type::kDel, op.key, "", {}};
      break;
    }
    case Type::kExpire: {
      if (FindLive(op.key) == map_.end()) break;
      result->flag = true;
      *record = Record{Record::Type::kExpire, op.key, "",
                       ToWallMs(ref.steady + ClampTtl(*op.ttl), ref)};
      break;
    }
    case Type::kPersist: {
      auto it = FindLive(op.key);
      if (it == map_.end() || !it->second.expires_at.has_value()) break;
      result->flag = true;
      *record = Record{Record::Type::kPersist, op.key, "", {}};
      break;
    }
    case Type::kAppend: {
      auto it = FindLive(op.key);
      size_t old_size = 0;
      if (it != map_.end()) {
        if (it->second.value.type() != KeyType::kString) return WrongType();
        old_size = it->second.value.str().size();
      }
      if (old_size > kMaxValueSize - op.arg.size()) {
        return Status(StatusCode::kInvalidArgument,
                      "value would be longer than kMaxValueSize");
      }
      result->count = old_size + op.arg.size();
      // A new key is a plain SET: replay must not depend on whether an
      // earlier, expired value is still around.
      *record =
          Record{it == map_.end() ? Record::Type::kSet : Record::Type::kAppend,
                 op.key, op.arg, std::nullopt};
      break;
    }
    case Type::kRename: {
      if (FindLive(op.key) == map_.end()) break;
      result->flag = true;
      if (op.key == op.arg) break;
      FindLive(op.arg);  // drops an expired destination
      *record = Record{Record::Type::kRename, op.key, op.arg, {}};
      break;
    }
    case Type::kIncr:
    case Type::kDecr: {
      auto it = FindLive(op.key);
      const std::string* current = nullptr;
      if (it != map_.end()) {
        if (it->second.value.type() != KeyType::kString) return WrongType();
        current = &it->second.value.str();
      }
      StatusOr<int64_t> added = Added(current, op.n1, op.type == Type::kDecr);
      if (!added.ok()) return added.status();
      result->integer = *added;
      // The resulting value, never the delta, so replay does no arithmetic.
      // A live key is REPLACEd so that its TTL is left exactly as it is.
      *record =
          Record{it == map_.end() ? Record::Type::kSet : Record::Type::kReplace,
                 op.key, std::to_string(*added), std::nullopt};
      break;
    }
    case Type::kHSet: {
      if (op.items.empty()) break;
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kHash, &it);
      if (!s.ok()) return s;
      const Hash* hash = it == map_.end() ? nullptr : &it->second.value.hash();
      std::set<std::string_view> new_fields;
      for (size_t i = 0; i < op.items.size(); i += 2) {
        if (hash == nullptr || hash->find(op.items[i]) == hash->end()) {
          new_fields.insert(op.items[i]);
        }
      }
      result->count = new_fields.size();
      Record r;
      r.type = Record::Type::kHSet;
      r.key = op.key;
      r.items = op.items;
      r.n1 = it == map_.end() ? 1 : 0;
      *record = std::move(r);
      break;
    }
    case Type::kHDel: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kHash, &it);
      if (!s.ok()) return s;
      if (it == map_.end()) break;
      const Hash& hash = it->second.value.hash();
      std::set<std::string_view> seen;
      Record r;
      r.type = Record::Type::kHDel;
      r.key = op.key;
      for (const std::string& field : op.items) {
        if (hash.find(field) != hash.end() && seen.insert(field).second) {
          r.items.push_back(field);
        }
      }
      if (r.items.empty()) break;
      result->count = r.items.size();
      *record = std::move(r);
      break;
    }
    case Type::kLPush:
    case Type::kRPush: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kList, &it);
      if (!s.ok()) return s;
      const size_t old_size =
          it == map_.end() ? 0 : it->second.value.list().size();
      result->count = old_size + op.items.size();
      if (op.items.empty()) break;
      Record r;
      r.type =
          op.type == Type::kLPush ? Record::Type::kLPush : Record::Type::kRPush;
      r.key = op.key;
      r.items = op.items;
      r.n1 = it == map_.end() ? 1 : 0;
      *record = std::move(r);
      break;
    }
    case Type::kLPop:
    case Type::kRPop: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kList, &it);
      if (!s.ok()) return s;
      if (it == map_.end()) break;
      const List& list = it->second.value.list();
      const size_t n = static_cast<size_t>(std::min<int64_t>(
          std::max<int64_t>(op.n1, 0), static_cast<int64_t>(list.size())));
      if (n == 0) break;
      for (size_t i = 0; i < n; ++i) {
        result->items.push_back(
            op.type == Type::kLPop ? list[i] : list[list.size() - 1 - i]);
      }
      result->count = n;
      Record r;
      r.type =
          op.type == Type::kLPop ? Record::Type::kLPop : Record::Type::kRPop;
      r.key = op.key;
      r.n1 = static_cast<int64_t>(n);
      *record = std::move(r);
      break;
    }
    case Type::kLSet: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kList, &it);
      if (!s.ok()) return s;
      if (it == map_.end()) break;
      size_t index;
      if (!ResolveIndex(op.n1, it->second.value.list().size(), &index)) break;
      result->flag = true;
      Record r;
      r.type = Record::Type::kLSet;
      r.key = op.key;
      r.items = {op.arg};
      r.n1 = static_cast<int64_t>(index);
      *record = std::move(r);
      break;
    }
    case Type::kLTrim: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kList, &it);
      if (!s.ok()) return s;
      if (it == map_.end()) break;
      const size_t size = it->second.value.list().size();
      size_t first = 0;
      size_t last = 0;
      Record r;
      r.type = Record::Type::kLTrim;
      r.key = op.key;
      if (ResolveRange(op.n1, op.n2, size, &first, &last)) {
        if (first == 0 && last == size - 1) break;  // keeps everything
        r.n1 = static_cast<int64_t>(first);
        r.n2 = static_cast<int64_t>(last);
      } else {
        r.n1 = 0;  // keeps nothing
        r.n2 = -1;
      }
      *record = std::move(r);
      break;
    }
    case Type::kSAdd: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kSet, &it);
      if (!s.ok()) return s;
      const StringSet* set =
          it == map_.end() ? nullptr : &it->second.value.set();
      std::set<std::string_view> seen;
      Record r;
      r.type = Record::Type::kSAdd;
      r.key = op.key;
      for (const std::string& member : op.items) {
        if ((set == nullptr || set->find(member) == set->end()) &&
            seen.insert(member).second) {
          r.items.push_back(member);
        }
      }
      if (r.items.empty()) break;  // everything was already there
      result->count = r.items.size();
      r.n1 = it == map_.end() ? 1 : 0;
      *record = std::move(r);
      break;
    }
    case Type::kSRem: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kSet, &it);
      if (!s.ok()) return s;
      if (it == map_.end()) break;
      const StringSet& set = it->second.value.set();
      std::set<std::string_view> seen;
      Record r;
      r.type = Record::Type::kSRem;
      r.key = op.key;
      for (const std::string& member : op.items) {
        if (set.find(member) != set.end() && seen.insert(member).second) {
          r.items.push_back(member);
        }
      }
      if (r.items.empty()) break;
      result->count = r.items.size();
      *record = std::move(r);
      break;
    }
    case Type::kZAdd: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kZSet, &it);
      if (!s.ok()) return s;
      const ZSet* zset = it == map_.end() ? nullptr : &it->second.value.zset();
      // If a member is repeated, the last score is the one that counts.
      std::map<std::string_view, double> wanted;
      for (size_t i = 0; i < op.items.size(); ++i) {
        wanted[op.items[i]] = op.scores[i];
      }
      Record r;
      r.type = Record::Type::kZAdd;
      r.key = op.key;
      size_t added = 0;
      for (const auto& [member, score] : wanted) {
        const double* existing = nullptr;
        if (zset != nullptr) {
          auto found = zset->scores.find(member);
          if (found != zset->scores.end()) existing = &found->second;
        }
        const bool is_new = existing == nullptr;
        if (is_new) ++added;
        if (is_new || *existing != score) {
          r.items.emplace_back(member);
          r.scores.push_back(score);
        }
      }
      if (r.items.empty()) break;  // every score is already what was asked
      result->count = added;
      r.n1 = it == map_.end() ? 1 : 0;
      *record = std::move(r);
      break;
    }
    case Type::kZRem: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kZSet, &it);
      if (!s.ok()) return s;
      if (it == map_.end()) break;
      const ZSet& zset = it->second.value.zset();
      std::set<std::string_view> seen;
      Record r;
      r.type = Record::Type::kZRem;
      r.key = op.key;
      for (const std::string& member : op.items) {
        if (zset.scores.find(member) != zset.scores.end() &&
            seen.insert(member).second) {
          r.items.push_back(member);
        }
      }
      if (r.items.empty()) break;
      result->count = r.items.size();
      *record = std::move(r);
      break;
    }
    case Type::kZIncr: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kZSet, &it);
      if (!s.ok()) return s;
      double current = 0;
      bool member_exists = false;
      if (it != map_.end()) {
        const ZSet& zset = it->second.value.zset();
        auto m = zset.scores.find(op.arg);
        if (m != zset.scores.end()) {
          current = m->second;
          member_exists = true;
        }
      }
      const double next = current + op.d;
      if (std::isnan(next)) {
        return Status(StatusCode::kInvalidArgument,
                      "the new score would not be a number");
      }
      result->number = next;
      if (member_exists && next == current) break;  // nothing changes
      Record r;
      r.type = Record::Type::kZAdd;
      r.key = op.key;
      r.items = {op.arg};
      r.scores = {next};
      r.n1 = it == map_.end() ? 1 : 0;
      *record = std::move(r);
      break;
    }
    case Type::kHIncr: {
      Map::iterator it;
      Status s = FindOfType(op.key, KeyType::kHash, &it);
      if (!s.ok()) return s;
      const std::string* current = nullptr;
      if (it != map_.end()) {
        const Hash& hash = it->second.value.hash();
        auto field = hash.find(op.arg);
        if (field != hash.end()) current = &field->second;
      }
      StatusOr<int64_t> added = Added(current, op.n1, /*subtract=*/false);
      if (!added.ok()) return added.status();
      result->integer = *added;
      Record r;
      r.type = Record::Type::kHSet;
      r.key = op.key;
      r.items = {op.arg, std::to_string(*added)};
      r.n1 = it == map_.end() ? 1 : 0;
      *record = std::move(r);
      break;
    }
  }
  return Status::Ok();
}

Status Db::FindOfType(std::string_view key, KeyType type, Map::iterator* it) {
  *it = FindLive(key);
  if (*it != map_.end() && (*it)->second.value.type() != type) {
    return WrongType();
  }
  return Status::Ok();
}

Db::Map::iterator Db::CollectionLocked(std::string_view key, KeyType type,
                                       bool create) {
  auto it = map_.find(key);
  if (it == map_.end()) {
    if (!create) return map_.end();
    it = InsertLocked(key);
    it->second.value = Value::Make(type);
    return it;
  }
  return it->second.value.type() == type ? it : map_.end();
}

Status Db::Write(const WriteBatch& batch) {
  if (batch.empty()) return Status::Ok();
  // Everything that can be checked without looking at the data comes first,
  // so that a bad batch is refused before anything is touched.
  for (const WriteBatch::Op& op : batch.ops_) {
    Status valid = ValidateOp(op);
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
  const ClockPoint ref = ReadClocks();  // one instant for the whole batch
  for (const WriteBatch::Op& op : batch.ops_) {
    std::optional<Record> record;
    OpResult unused;
    Status s = PlanLocked(op, ref, &record, &unused);
    if (!s.ok()) {
      RollbackLocked(std::move(undo));
      return s;
    }
    if (!record.has_value()) continue;

    RememberLocked(&undo, record->key);
    if (record->type == Record::Type::kRename) {
      RememberLocked(&undo, record->value);
    }
    // Applied through the same code as replay, so a live batch and a
    // recovered one cannot differ.
    Record copy = *record;
    ApplyRecordLocked(std::move(copy), ref);
    effective.push_back(std::move(*record));
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
    RollbackLocked(std::move(undo));
    return s;
  }
  return Status::Ok();
}

void Db::RememberLocked(Undo* undo, std::string_view key) {
  if (undo->find(key) != undo->end()) return;
  auto it = map_.find(key);
  undo->emplace(std::string(key),
                it == map_.end() ? std::nullopt
                                 : std::optional<Entry>(it->second.Clone()));
}

void Db::RollbackLocked(Undo&& undo) {
  for (auto& [key, original] : undo) {
    auto it = map_.find(key);
    if (!original.has_value()) {
      if (it != map_.end()) EraseLocked(it);
      continue;
    }
    if (it == map_.end()) it = InsertLocked(key);
    ClearExpiryLocked(it);
    it->second.value = std::move(original->value);
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
    if (it == map_.end() || it->second.value.type() != KeyType::kString) {
      values.emplace_back(std::nullopt);
    } else {
      values.emplace_back(it->second.value.str());
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
  const ClockPoint ref = ReadClocks();
  const Clock::time_point now = ref.steady;
  Status s = storage_->Replace([&](File& out) -> Status {
    // Batch records so that a big database is not written one call per key.
    std::string buf = EncodeHeader();
    if (!empty) {
      for (const auto& [key, entry] : map_) {
        if (entry.expires_at.has_value() && *entry.expires_at <= now) continue;
        Status written = WriteEntryRecords(key, entry, ref, &buf, out);
        if (!written.ok()) return written;
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

Status Db::WriteEntryRecords(const std::string& key, const Entry& entry,
                             const ClockPoint& ref, std::string* buf,
                             File& out) {
  constexpr size_t kFlushBytes = 1 << 20;
  // Collections are written in pieces so that no record outgrows the limit.
  const size_t chunk_bytes =
      static_cast<size_t>(std::max<uint64_t>(options_.compact_chunk_bytes, 1));

  auto emit = [&](const Record& record) -> Status {
    Status s = EncodeRecord(record, buf);
    if (!s.ok()) return s;
    if (buf->size() >= kFlushBytes) {
      s = out.Append(*buf);
      buf->clear();
    }
    return s;
  };

  const Value& value = entry.value;
  switch (value.type()) {
    case KeyType::kString: {
      // A string carries its own expiry.
      Record record{Record::Type::kSet, key, value.str(), std::nullopt};
      if (entry.expires_at.has_value()) {
        record.expires_at_ms = ToWallMs(*entry.expires_at, ref);
      }
      return emit(record);
    }
    case KeyType::kHash:
    case KeyType::kList:
    case KeyType::kSet:
    case KeyType::kZSet: {
      Record chunk;
      switch (value.type()) {
        case KeyType::kHash:
          chunk.type = Record::Type::kHSet;
          break;
        case KeyType::kSet:
          chunk.type = Record::Type::kSAdd;
          break;
        case KeyType::kZSet:
          chunk.type = Record::Type::kZAdd;
          break;
        default:
          chunk.type = Record::Type::kRPush;
          break;
      }
      chunk.key = key;
      chunk.n1 = 1;  // the first chunk starts the value; later ones add to it
      size_t bytes = 0;
      auto flush = [&]() -> Status {
        if (chunk.items.empty()) return Status::Ok();
        Status s = emit(chunk);
        chunk.items.clear();
        chunk.scores.clear();
        chunk.n1 = 0;
        bytes = 0;
        return s;
      };
      // Adds one element (a hash's field and value stay together, a member
      // keeps its score) and writes a chunk out once it is big enough.
      auto add = [&](const std::string& a, const std::string* b,
                     const double* score) -> Status {
        chunk.items.push_back(a);
        bytes += a.size() + 4;
        if (b != nullptr) {
          chunk.items.push_back(*b);
          bytes += b->size() + 4;
        }
        if (score != nullptr) {
          chunk.scores.push_back(*score);
          bytes += 8;
        }
        return bytes >= chunk_bytes ? flush() : Status::Ok();
      };
      Status s = Status::Ok();
      switch (value.type()) {
        case KeyType::kHash:
          for (const auto& [field, field_value] : value.hash()) {
            s = add(field, &field_value, nullptr);
            if (!s.ok()) return s;
          }
          break;
        case KeyType::kSet:
          for (const std::string& member : value.set()) {
            s = add(member, nullptr, nullptr);
            if (!s.ok()) return s;
          }
          break;
        case KeyType::kZSet:
          for (const auto& [member, score] : value.zset().scores) {
            s = add(member, nullptr, &score);
            if (!s.ok()) return s;
          }
          break;
        default:
          for (const std::string& element : value.list()) {
            s = add(element, nullptr, nullptr);
            if (!s.ok()) return s;
          }
          break;
      }
      s = flush();
      if (!s.ok()) return s;
      break;
    }
    default:
      return Status(StatusCode::kInvalidArgument, "unsupported value type");
  }
  if (entry.expires_at.has_value()) {
    return emit(Record{Record::Type::kExpire, key, "",
                       ToWallMs(*entry.expires_at, ref)});
  }
  return Status::Ok();
}

void Db::SetCompactThreshold(uint64_t base_size) {
  compact_threshold_ = std::max<uint64_t>(options_.compact_min_bytes,
                                          base_size * options_.compact_ratio);
}

void Db::ApplyRecordLocked(Record&& record, const ClockPoint& ref) {
  switch (record.type) {
    case Record::Type::kSet: {
      auto it = InsertLocked(record.key);
      ClearExpiryLocked(it);
      it->second.value = Value(std::move(record.value));
      if (record.expires_at_ms.has_value()) {
        SetExpiryLocked(it, FromWallMs(*record.expires_at_ms, ref));
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
        SetExpiryLocked(it, FromWallMs(*record.expires_at_ms, ref));
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
      if (it->second.value.type() == KeyType::kString) {
        it->second.value.str().append(record.value);
      }
      break;
    }
    case Record::Type::kHSet: {
      Map::iterator it;
      if (record.n1 == 1) {
        // The key was new: whatever the log still has under this name is a
        // stale entry that had already expired.
        it = InsertLocked(record.key);
        ClearExpiryLocked(it);
        it->second.value = Value::Make(KeyType::kHash);
      } else {
        it = CollectionLocked(record.key, KeyType::kHash, /*create=*/true);
        if (it == map_.end()) break;
      }
      Hash& hash = it->second.value.hash();
      for (size_t i = 0; i + 1 < record.items.size(); i += 2) {
        hash[std::move(record.items[i])] = std::move(record.items[i + 1]);
      }
      break;
    }
    case Record::Type::kHDel: {
      auto it = CollectionLocked(record.key, KeyType::kHash, /*create=*/false);
      if (it == map_.end()) break;
      Hash& hash = it->second.value.hash();
      for (const std::string& field : record.items) hash.erase(field);
      if (hash.empty()) EraseLocked(it);
      break;
    }
    case Record::Type::kLPush:
    case Record::Type::kRPush: {
      Map::iterator it;
      if (record.n1 == 1) {
        it = InsertLocked(record.key);
        ClearExpiryLocked(it);
        it->second.value = Value::Make(KeyType::kList);
      } else {
        it = CollectionLocked(record.key, KeyType::kList, /*create=*/true);
        if (it == map_.end()) break;
      }
      List& list = it->second.value.list();
      for (std::string& item : record.items) {
        if (record.type == Record::Type::kLPush) {
          list.push_front(std::move(item));
        } else {
          list.push_back(std::move(item));
        }
      }
      break;
    }
    case Record::Type::kLPop:
    case Record::Type::kRPop: {
      auto it = CollectionLocked(record.key, KeyType::kList, /*create=*/false);
      if (it == map_.end()) break;
      List& list = it->second.value.list();
      for (int64_t i = 0; i < record.n1 && !list.empty(); ++i) {
        if (record.type == Record::Type::kLPop) {
          list.pop_front();
        } else {
          list.pop_back();
        }
      }
      if (list.empty()) EraseLocked(it);
      break;
    }
    case Record::Type::kLSet: {
      auto it = CollectionLocked(record.key, KeyType::kList, /*create=*/false);
      if (it == map_.end()) break;
      List& list = it->second.value.list();
      if (static_cast<size_t>(record.n1) < list.size()) {
        list[static_cast<size_t>(record.n1)] = std::move(record.items[0]);
      }
      break;
    }
    case Record::Type::kLTrim: {
      auto it = CollectionLocked(record.key, KeyType::kList, /*create=*/false);
      if (it == map_.end()) break;
      List& list = it->second.value.list();
      if (record.n2 < record.n1 ||
          static_cast<size_t>(record.n1) >= list.size()) {
        EraseLocked(it);
        break;
      }
      const size_t last =
          std::min<size_t>(static_cast<size_t>(record.n2), list.size() - 1);
      list.erase(list.begin() + static_cast<std::ptrdiff_t>(last) + 1,
                 list.end());
      list.erase(list.begin(),
                 list.begin() + static_cast<std::ptrdiff_t>(record.n1));
      break;
    }
    case Record::Type::kSAdd: {
      Map::iterator it;
      if (record.n1 == 1) {
        it = InsertLocked(record.key);
        ClearExpiryLocked(it);
        it->second.value = Value::Make(KeyType::kSet);
      } else {
        it = CollectionLocked(record.key, KeyType::kSet, /*create=*/true);
        if (it == map_.end()) break;
      }
      StringSet& set = it->second.value.set();
      for (std::string& member : record.items) set.insert(std::move(member));
      break;
    }
    case Record::Type::kSRem: {
      auto it = CollectionLocked(record.key, KeyType::kSet, /*create=*/false);
      if (it == map_.end()) break;
      StringSet& set = it->second.value.set();
      for (const std::string& member : record.items) set.erase(member);
      if (set.empty()) EraseLocked(it);
      break;
    }
    case Record::Type::kZAdd: {
      Map::iterator it;
      if (record.n1 == 1) {
        it = InsertLocked(record.key);
        ClearExpiryLocked(it);
        it->second.value = Value::Make(KeyType::kZSet);
      } else {
        it = CollectionLocked(record.key, KeyType::kZSet, /*create=*/true);
        if (it == map_.end()) break;
      }
      ZSet& zset = it->second.value.zset();
      for (size_t i = 0; i < record.items.size(); ++i) {
        zset.Set(record.items[i], record.scores[i]);
      }
      break;
    }
    case Record::Type::kZRem: {
      auto it = CollectionLocked(record.key, KeyType::kZSet, /*create=*/false);
      if (it == map_.end()) break;
      ZSet& zset = it->second.value.zset();
      for (const std::string& member : record.items) zset.Remove(member);
      if (zset.size() == 0) EraseLocked(it);
      break;
    }
    case Record::Type::kReplace: {
      auto it = InsertLocked(record.key);  // the TTL, if any, is kept
      it->second.value = Value(std::move(record.value));
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
        ApplyRecordLocked(std::move(member), ref);
      }
      break;
    case Record::Type::kXAdd:
    case Record::Type::kXTrim:
      // The Db does not have streams yet. Nothing writes these records.
      break;
  }
}

Db::ClockPoint Db::ReadClocks() {
  return {now_(), std::chrono::duration_cast<std::chrono::milliseconds>(
                      wall_now_().time_since_epoch())
                      .count()};
}

int64_t Db::ToWallMs(Clock::time_point at, const ClockPoint& ref) {
  // Work in whole milliseconds. `at` is at most kMaxTtl from `ref`, so none
  // of this can overflow.
  const int64_t delta_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(at - ref.steady)
          .count();
  return std::max<int64_t>(ref.wall_ms + delta_ms, 0);
}

Db::Clock::time_point Db::FromWallMs(int64_t unix_ms, const ClockPoint& ref) {
  // The log may hold any value, so saturate instead of overflowing: an expiry
  // further away than kMaxTtl is treated as kMaxTtl away.
  const int64_t max_ms = kMaxTtl.count();
  int64_t delta_ms;
  if (__builtin_sub_overflow(unix_ms, ref.wall_ms, &delta_ms)) {
    delta_ms = max_ms;
  }
  delta_ms = std::clamp<int64_t>(delta_ms, -max_ms, max_ms);
  return ref.steady + std::chrono::milliseconds(delta_ms);
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
