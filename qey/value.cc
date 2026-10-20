#include "qey/value.h"

#include <charconv>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

StreamId StreamId::Next() const {
  if (seq < kMaxStreamIdPart) return {ms, seq + 1};
  if (ms < kMaxStreamIdPart) return {ms + 1, 0};
  return *this;
}

std::string StreamId::ToString() const {
  return std::to_string(ms) + "-" + std::to_string(seq);
}

namespace {

// Reads a whole string of digits as a number no larger than kMaxStreamIdPart.
bool ParseIdPart(std::string_view text, uint64_t* out) {
  if (text.empty() || text.front() < '0' || text.front() > '9') return false;
  auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), *out);
  return ec == std::errc() && ptr == text.data() + text.size() &&
         *out <= kMaxStreamIdPart;
}

}  // namespace

std::optional<StreamId> StreamId::Parse(std::string_view text) {
  StreamId id;
  const size_t dash = text.find('-');
  if (!ParseIdPart(text.substr(0, dash), &id.ms)) return std::nullopt;
  if (dash != std::string_view::npos &&
      !ParseIdPart(text.substr(dash + 1), &id.seq)) {
    return std::nullopt;
  }
  return id;
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
    case KeyType::kStream:
      v.data_ = std::make_unique<Stream>();
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
    case KeyType::kStream:
      copy.data_ = std::make_unique<Stream>(stream());
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
    case 4:
      return KeyType::kZSet;
    default:
      return KeyType::kStream;
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
    case KeyType::kStream:
      return stream().entries.size();
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
    case KeyType::kStream:
      // A payload, its length prefix, the flag, the ID and the record's own
      // header and key.
      for (const StreamEntry& e : stream().entries)
        bytes += e.payload.size() + 40;
      break;
    case KeyType::kNone:
      break;
  }
  return bytes;
}

}  // namespace qey
