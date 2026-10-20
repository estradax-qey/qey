#ifndef QEY_WRITE_BATCH_H_
#define QEY_WRITE_BATCH_H_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qey/value.h"

namespace qey {

// A list of changes to apply together with Db::Write(). Building a batch does
// not touch any Db, and a batch can be written to more than one Db or
// written again.
//
// The changes are applied in the order they were added, each seeing the
// effects of the ones before it, and take effect as a unit:
//  - other threads see all of them or none;
//  - after a crash, a persistent Db has all of them or none;
//  - if one fails, none are applied.
//
// Changes that name a missing key do nothing, like their Db counterparts do
// when they return false. Incr() and Decr() fail the whole batch if the value
// is not an integer or would overflow.
class WriteBatch {
 public:
  // The same as Db::Set(), Db::Del(), Db::Expire(), Db::Persist(),
  // Db::Append(), Db::Rename(), Db::Incr() and Db::Decr().
  WriteBatch& Set(std::string_view key, std::string_view value,
                  std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
    Op op = MakeOp(Op::Type::kSet, key, value);
    op.ttl = ttl;
    return Add(std::move(op));
  }
  WriteBatch& Del(std::string_view key) {
    return Add(MakeOp(Op::Type::kDel, key));
  }
  WriteBatch& Expire(std::string_view key, std::chrono::milliseconds ttl) {
    Op op = MakeOp(Op::Type::kExpire, key);
    op.ttl = ttl;
    return Add(std::move(op));
  }
  WriteBatch& Persist(std::string_view key) {
    return Add(MakeOp(Op::Type::kPersist, key));
  }
  WriteBatch& Append(std::string_view key, std::string_view suffix) {
    return Add(MakeOp(Op::Type::kAppend, key, suffix));
  }
  WriteBatch& Rename(std::string_view from, std::string_view to) {
    return Add(MakeOp(Op::Type::kRename, from, to));
  }
  WriteBatch& Incr(std::string_view key, int64_t delta = 1) {
    Op op = MakeOp(Op::Type::kIncr, key);
    op.n1 = delta;
    return Add(std::move(op));
  }
  WriteBatch& Decr(std::string_view key, int64_t delta = 1) {
    Op op = MakeOp(Op::Type::kDecr, key);
    op.n1 = delta;
    return Add(std::move(op));
  }

  // Hashes. The same as Db::HSet(), Db::HDel() and Db::HIncr().
  WriteBatch& HSet(std::string_view key, std::string_view field,
                   std::string_view value) {
    return HSet(
        key, std::vector<FieldValue>{{std::string(field), std::string(value)}});
  }
  WriteBatch& HSet(std::string_view key,
                   const std::vector<FieldValue>& fields) {
    Op op = MakeOp(Op::Type::kHSet, key);
    for (const FieldValue& fv : fields) {
      op.items.push_back(fv.first);
      op.items.push_back(fv.second);
    }
    return Add(std::move(op));
  }
  WriteBatch& HDel(std::string_view key,
                   const std::vector<std::string>& fields) {
    Op op = MakeOp(Op::Type::kHDel, key);
    op.items = fields;
    return Add(std::move(op));
  }
  WriteBatch& HIncr(std::string_view key, std::string_view field,
                    int64_t delta = 1) {
    Op op = MakeOp(Op::Type::kHIncr, key, field);
    op.n1 = delta;
    return Add(std::move(op));
  }

  // Lists. The same as Db::LPush(), Db::RPush(), Db::LPop(), Db::RPop(),
  // Db::LSet() and Db::LTrim(). (A pop inside a batch discards what it
  // removes.)
  WriteBatch& LPush(std::string_view key,
                    const std::vector<std::string>& values) {
    Op op = MakeOp(Op::Type::kLPush, key);
    op.items = values;
    return Add(std::move(op));
  }
  WriteBatch& RPush(std::string_view key,
                    const std::vector<std::string>& values) {
    Op op = MakeOp(Op::Type::kRPush, key);
    op.items = values;
    return Add(std::move(op));
  }
  WriteBatch& LPop(std::string_view key, size_t count = 1) {
    Op op = MakeOp(Op::Type::kLPop, key);
    op.n1 = static_cast<int64_t>(std::min<size_t>(count, INT64_MAX));
    return Add(std::move(op));
  }
  WriteBatch& RPop(std::string_view key, size_t count = 1) {
    Op op = MakeOp(Op::Type::kRPop, key);
    op.n1 = static_cast<int64_t>(std::min<size_t>(count, INT64_MAX));
    return Add(std::move(op));
  }
  WriteBatch& LSet(std::string_view key, int64_t index,
                   std::string_view value) {
    Op op = MakeOp(Op::Type::kLSet, key, value);
    op.n1 = index;
    return Add(std::move(op));
  }
  WriteBatch& LTrim(std::string_view key, int64_t start, int64_t stop) {
    Op op = MakeOp(Op::Type::kLTrim, key);
    op.n1 = start;
    op.n2 = stop;
    return Add(std::move(op));
  }

  // Sets. The same as Db::SAdd() and Db::SRem().
  WriteBatch& SAdd(std::string_view key,
                   const std::vector<std::string>& members) {
    Op op = MakeOp(Op::Type::kSAdd, key);
    op.items = members;
    return Add(std::move(op));
  }
  WriteBatch& SRem(std::string_view key,
                   const std::vector<std::string>& members) {
    Op op = MakeOp(Op::Type::kSRem, key);
    op.items = members;
    return Add(std::move(op));
  }

  // Sorted sets. The same as Db::ZAdd(), Db::ZRem() and Db::ZIncr().
  WriteBatch& ZAdd(std::string_view key,
                   const std::vector<ScoredMember>& members) {
    Op op = MakeOp(Op::Type::kZAdd, key);
    for (const ScoredMember& m : members) {
      op.items.push_back(m.member);
      op.scores.push_back(m.score);
    }
    return Add(std::move(op));
  }
  WriteBatch& ZRem(std::string_view key,
                   const std::vector<std::string>& members) {
    Op op = MakeOp(Op::Type::kZRem, key);
    op.items = members;
    return Add(std::move(op));
  }
  WriteBatch& ZIncr(std::string_view key, std::string_view member,
                    double delta = 1) {
    Op op = MakeOp(Op::Type::kZIncr, key, member);
    op.d = delta;
    return Add(std::move(op));
  }

  // Streams. The same as Db::XAdd(), Db::XTrimMaxLen() and Db::XTrimMinId().
  // The ID an added entry gets is not reported by Write(); read it with
  // Db::XLastId() afterwards, or use Db::XAdd() when you need it right away.
  // Entries added by one batch all get IDs from the clock reading taken when
  // the batch is applied, so they differ only in their sequence numbers.
  WriteBatch& XAdd(std::string_view key, std::string_view payload) {
    return Add(MakeOp(Op::Type::kXAdd, key, payload));
  }
  WriteBatch& XTrimMaxLen(std::string_view key, size_t max_len) {
    Op op = MakeOp(Op::Type::kXTrimMaxLen, key);
    op.n1 = static_cast<int64_t>(std::min<size_t>(max_len, INT64_MAX));
    return Add(std::move(op));
  }
  WriteBatch& XTrimMinId(std::string_view key, StreamId min_id) {
    Op op = MakeOp(Op::Type::kXTrimMinId, key);
    // An ID part above INT64_MAX goes in as a negative number, which Write()
    // refuses.
    op.n1 = static_cast<int64_t>(min_id.ms);
    op.n2 = static_cast<int64_t>(min_id.seq);
    return Add(std::move(op));
  }

  // Removes all changes from the batch.
  void Clear() { ops_.clear(); }

  size_t size() const { return ops_.size(); }
  bool empty() const { return ops_.empty(); }

 private:
  friend class Db;

  // One change. Db builds these for its single-key calls as well, so that a
  // call and the same change inside a batch share one implementation.
  struct Op {
    enum class Type {
      kSet,
      kDel,
      kExpire,
      kPersist,
      kAppend,
      kRename,
      kIncr,
      kDecr,
      kHSet,
      kHDel,
      kHIncr,
      kLPush,
      kRPush,
      kLPop,
      kRPop,
      kLSet,
      kLTrim,
      kSAdd,
      kSRem,
      kZAdd,
      kZRem,
      kZIncr,
      kXAdd,
      kXTrimMaxLen,
      kXTrimMinId,
    };

    Type type = Type::kSet;
    std::string key;
    // The value for kSet, the suffix for kAppend, the new key for kRename.
    std::string arg;
    std::optional<std::chrono::milliseconds> ttl;  // kSet, kExpire.
    // kHSet: fields and values, alternating. kHDel: the fields. kLPush and
    // kRPush: the values. kSAdd and kSRem: the members. kXAdd: `arg` is the
    // payload.
    std::vector<std::string> items;
    // kIncr, kDecr, kHIncr: the delta. kLPop, kRPop: how many. kLSet: the
    // index. kLTrim: the first index to keep. kXTrimMaxLen: the most entries
    // to keep. kXTrimMinId: the milliseconds of the smallest ID to keep.
    int64_t n1 = 0;
    // kLTrim: the last index to keep. kXTrimMinId: the sequence number of the
    // smallest ID to keep.
    int64_t n2 = 0;
    // kZAdd: the score of each member in `items`. kZIncr: `d` is the delta.
    std::vector<double> scores;
    double d = 0;
  };

  static Op MakeOp(Op::Type type, std::string_view key,
                   std::string_view arg = "") {
    Op op;
    op.type = type;
    op.key = std::string(key);
    op.arg = std::string(arg);
    return op;
  }

  WriteBatch& Add(Op op) {
    ops_.push_back(std::move(op));
    return *this;
  }

  std::vector<Op> ops_;
};

}  // namespace qey

#endif  // QEY_WRITE_BATCH_H_
