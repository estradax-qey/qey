#ifndef QEY_WRITE_BATCH_H_
#define QEY_WRITE_BATCH_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    return Add(Op::Type::kSet, key, value, ttl, 0);
  }
  WriteBatch& Del(std::string_view key) {
    return Add(Op::Type::kDel, key, "", std::nullopt, 0);
  }
  WriteBatch& Expire(std::string_view key, std::chrono::milliseconds ttl) {
    return Add(Op::Type::kExpire, key, "", ttl, 0);
  }
  WriteBatch& Persist(std::string_view key) {
    return Add(Op::Type::kPersist, key, "", std::nullopt, 0);
  }
  WriteBatch& Append(std::string_view key, std::string_view suffix) {
    return Add(Op::Type::kAppend, key, suffix, std::nullopt, 0);
  }
  WriteBatch& Rename(std::string_view from, std::string_view to) {
    return Add(Op::Type::kRename, from, to, std::nullopt, 0);
  }
  WriteBatch& Incr(std::string_view key, int64_t delta = 1) {
    return Add(Op::Type::kIncr, key, "", std::nullopt, delta);
  }
  WriteBatch& Decr(std::string_view key, int64_t delta = 1) {
    return Add(Op::Type::kDecr, key, "", std::nullopt, delta);
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
    };

    Type type = Type::kSet;
    std::string key;
    // The value for kSet, the suffix for kAppend, the new key for kRename.
    std::string arg;
    std::optional<std::chrono::milliseconds> ttl;  // kSet, kExpire.
    int64_t n1 = 0;                                // kIncr, kDecr: the delta.
  };

  WriteBatch& Add(Op::Type type, std::string_view key, std::string_view arg,
                  std::optional<std::chrono::milliseconds> ttl, int64_t n1) {
    Op op;
    op.type = type;
    op.key = std::string(key);
    op.arg = std::string(arg);
    op.ttl = ttl;
    op.n1 = n1;
    ops_.push_back(std::move(op));
    return *this;
  }

  std::vector<Op> ops_;
};

}  // namespace qey

#endif  // QEY_WRITE_BATCH_H_
