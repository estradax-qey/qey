#include "qey/value.h"

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace qey {

bool ZSet::Set(std::string_view member, double score) {
  auto it = scores.find(member);
  if (it == scores.end()) {
    auto inserted = scores.emplace(std::string(member), score).first;
    order.emplace(score, inserted->first);
    return true;
  }
  if (it->second != score) {
    order.erase({it->second, it->first});
    it->second = score;
    order.emplace(score, it->first);
  }
  return false;
}

bool ZSet::Remove(std::string_view member) {
  auto it = scores.find(member);
  if (it == scores.end()) return false;
  order.erase({it->second, it->first});
  scores.erase(it);
  return true;
}

Value Value::Make(KeyType type) {
  Value v;
  switch (type) {
    case KeyType::kList:
      v.data_ = std::make_unique<List>();
      break;
    case KeyType::kHash:
      v.data_ = std::make_unique<Hash>();
      break;
    case KeyType::kSet:
      v.data_ = std::make_unique<StringSet>();
      break;
    case KeyType::kZSet:
      v.data_ = std::make_unique<ZSet>();
      break;
    case KeyType::kNone:
    case KeyType::kString:
      break;
  }
  return v;
}

Value Value::Clone() const {
  Value copy;
  switch (type()) {
    case KeyType::kString:
      copy.data_ = str();
      break;
    case KeyType::kList:
      copy.data_ = std::make_unique<List>(list());
      break;
    case KeyType::kHash:
      copy.data_ = std::make_unique<Hash>(hash());
      break;
    case KeyType::kSet:
      copy.data_ = std::make_unique<StringSet>(set());
      break;
    case KeyType::kZSet:
      copy.data_ = std::make_unique<ZSet>(zset());
      break;
    case KeyType::kNone:
      break;
  }
  return copy;
}

KeyType Value::type() const {
  switch (data_.index()) {
    case 0:
      return KeyType::kString;
    case 1:
      return KeyType::kList;
    case 2:
      return KeyType::kHash;
    case 3:
      return KeyType::kSet;
    default:
      return KeyType::kZSet;
  }
}

size_t Value::element_count() const {
  switch (type()) {
    case KeyType::kList:
      return list().size();
    case KeyType::kHash:
      return hash().size();
    case KeyType::kSet:
      return set().size();
    case KeyType::kZSet:
      return zset().size();
    default:
      return 0;
  }
}

size_t Value::ApproxEncodedBytes() const {
  // Per element: a length prefix, and for a hash a second one; sorted sets
  // also carry an 8-byte score.
  size_t bytes = 0;
  switch (type()) {
    case KeyType::kString:
      return str().size();
    case KeyType::kList:
      for (const std::string& v : list()) bytes += v.size() + 4;
      break;
    case KeyType::kHash:
      for (const auto& [f, v] : hash()) bytes += f.size() + v.size() + 8;
      break;
    case KeyType::kSet:
      for (const std::string& m : set()) bytes += m.size() + 4;
      break;
    case KeyType::kZSet:
      for (const auto& [m, score] : zset().scores) bytes += m.size() + 12;
      break;
    case KeyType::kNone:
      break;
  }
  return bytes;
}

}  // namespace qey
