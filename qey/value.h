#ifndef QEY_VALUE_H_
#define QEY_VALUE_H_

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace qey {

// What a key holds.
enum class KeyType {
  kNone,  // The key does not exist.
  kString,
  kList,
  kHash,
  kSet,
  kZSet,
};

// A hash field and its value.
using FieldValue = std::pair<std::string, std::string>;

using List = std::deque<std::string>;
// Ordered, so that listing a hash or set is deterministic. The transparent
// comparison lets lookups take a string_view without allocating.
using Hash = std::map<std::string, std::string, std::less<>>;
using StringSet = std::set<std::string, std::less<>>;

// A sorted set: members with scores, ordered by (score, member).
struct ZSet {
  std::map<std::string, double, std::less<>> scores;  // member -> score
  std::set<std::pair<double, std::string>> order;     // (score, member)

  size_t size() const { return scores.size(); }

  // Sets the score of `member`. Returns true if the member is new.
  bool Set(std::string_view member, double score);

  // Removes `member`. Returns true if it was there.
  bool Remove(std::string_view member);
};

// The value stored under a key: a string or one of the collection types.
// Collections are held by pointer so that a string, by far the commonest
// value, does not pay for the size of all the alternatives.
class Value {
 public:
  Value() = default;  // an empty string
  explicit Value(std::string s) : data_(std::move(s)) {}

  // An empty value of the given collection type.
  static Value Make(KeyType type);

  Value(Value&&) noexcept = default;
  Value& operator=(Value&&) noexcept = default;
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;

  // A deep copy.
  Value Clone() const;

  KeyType type() const;

  // The contents. The type must match: check type() first.
  std::string& str() { return std::get<std::string>(data_); }
  const std::string& str() const { return std::get<std::string>(data_); }
  List& list() { return *std::get<std::unique_ptr<List>>(data_); }
  const List& list() const { return *std::get<std::unique_ptr<List>>(data_); }
  Hash& hash() { return *std::get<std::unique_ptr<Hash>>(data_); }
  const Hash& hash() const { return *std::get<std::unique_ptr<Hash>>(data_); }
  StringSet& set() { return *std::get<std::unique_ptr<StringSet>>(data_); }
  const StringSet& set() const {
    return *std::get<std::unique_ptr<StringSet>>(data_);
  }
  ZSet& zset() { return *std::get<std::unique_ptr<ZSet>>(data_); }
  const ZSet& zset() const { return *std::get<std::unique_ptr<ZSet>>(data_); }

  // Number of elements of a collection; 0 for a string.
  size_t element_count() const;

  // Roughly how many bytes this takes to write to the log.
  size_t ApproxEncodedBytes() const;

 private:
  std::variant<std::string, std::unique_ptr<List>, std::unique_ptr<Hash>,
               std::unique_ptr<StringSet>, std::unique_ptr<ZSet>>
      data_;
};

}  // namespace qey

#endif  // QEY_VALUE_H_
