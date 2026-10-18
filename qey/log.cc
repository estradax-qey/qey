#include "qey/log.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "qey/crc32.h"
#include "qey/file.h"
#include "qey/status.h"

namespace qey {
namespace {

constexpr size_t kFrameHeaderSize = 8;  // length:u32 crc32:u32

void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>(v >> (8 * i)));
}

void PutI64(std::string* out, int64_t v) {
  const uint64_t u = static_cast<uint64_t>(v);
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>(u >> (8 * i)));
}

void PutString(std::string* out, std::string_view s) {
  PutU32(out, static_cast<uint32_t>(s.size()));
  out->append(s);
}

void PutStrings(std::string* out, const std::vector<std::string>& strings) {
  PutU32(out, static_cast<uint32_t>(strings.size()));
  for (const std::string& s : strings) PutString(out, s);
}

uint32_t GetU32(const char* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(p[i]);
  return v;
}

// Decodes the fields of a record payload.
class PayloadReader {
 public:
  explicit PayloadReader(std::string_view data) : data_(data) {}

  bool ReadI64(int64_t* out) {
    if (data_.size() < 8) return false;
    uint64_t u = 0;
    for (int i = 7; i >= 0; --i) u = (u << 8) | static_cast<uint8_t>(data_[i]);
    *out = static_cast<int64_t>(u);
    data_.remove_prefix(8);
    return true;
  }

  bool ReadU32(uint32_t* out) {
    if (data_.size() < 4) return false;
    *out = GetU32(data_.data());
    data_.remove_prefix(4);
    return true;
  }

  bool ReadBytes(size_t n, std::string_view* out) {
    if (data_.size() < n) return false;
    *out = data_.substr(0, n);
    data_.remove_prefix(n);
    return true;
  }

  bool ReadU8(uint8_t* out) {
    if (data_.empty()) return false;
    *out = static_cast<uint8_t>(data_[0]);
    data_.remove_prefix(1);
    return true;
  }

  // Reads a count and then that many strings.
  bool ReadStrings(std::vector<std::string>* out) {
    uint32_t count;
    if (!ReadU32(&count)) return false;
    // Every string takes at least 4 bytes, so a larger count cannot be real.
    if (count > data_.size() / 4) return false;
    out->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      out->emplace_back();
      if (!ReadString(&out->back())) return false;
    }
    return true;
  }

  bool ReadString(std::string* out) {
    if (data_.size() < 4) return false;
    const uint32_t n = GetU32(data_.data());
    data_.remove_prefix(4);
    if (data_.size() < n) return false;
    out->assign(data_.substr(0, n));
    data_.remove_prefix(n);
    return true;
  }

  bool done() const { return data_.empty(); }

 private:
  std::string_view data_;
};

// How a collection record is laid out. After the key come, in this order: a
// "fresh" flag byte, a list of strings, and up to two integers.
struct Shape {
  bool fresh = false;
  bool strings = false;
  size_t strings_multiple = 1;  // the count must be a multiple of this...
  size_t strings_exact = 0;     // ...and, if non-zero, exactly this
  int ints = 0;                 // 0, 1 (n1) or 2 (n1 and n2)
  int64_t n1_min = 0;
  int64_t n2_min = 0;
};

// Returns the layout of a collection record type, or null for other types.
const Shape* ShapeOf(Record::Type type) {
  using T = Record::Type;
  static const Shape kPairs{
      .fresh = true, .strings = true, .strings_multiple = 2};
  static const Shape kFreshStrings{.fresh = true, .strings = true};
  static const Shape kStrings{.strings = true};
  static const Shape kCount{.ints = 1, .n1_min = 1};
  static const Shape kIndexed{.strings = true, .strings_exact = 1, .ints = 1};
  static const Shape kRange{.ints = 2, .n2_min = -1};
  switch (type) {
    case T::kHSet:
      return &kPairs;
    case T::kHDel:
    case T::kSRem:
      return &kStrings;
    case T::kLPush:
    case T::kRPush:
    case T::kSAdd:
      return &kFreshStrings;
    case T::kLPop:
    case T::kRPop:
      return &kCount;
    case T::kLSet:
      return &kIndexed;
    case T::kLTrim:
      return &kRange;
    default:
      return nullptr;
  }
}

// Whether the fields of `record` fit the layout of its type.
bool FitsShape(const Shape& shape, const Record& record) {
  if (shape.fresh && (record.n1 < 0 || record.n1 > 1)) return false;
  if (shape.strings) {
    const size_t n = record.items.size();
    if (n == 0 || n % shape.strings_multiple != 0) return false;
    if (shape.strings_exact != 0 && n != shape.strings_exact) return false;
  } else if (!record.items.empty()) {
    return false;
  }
  if (shape.ints >= 1 && record.n1 < shape.n1_min) return false;
  if (shape.ints >= 2 && record.n2 < shape.n2_min) return false;
  // Fields the layout does not use must be left alone.
  if (!shape.fresh && shape.ints == 0 && record.n1 != 0) return false;
  if (shape.ints < 2 && record.n2 != 0) return false;
  return true;
}

bool HasValue(Record::Type type) {
  return type == Record::Type::kSet || type == Record::Type::kAppend ||
         type == Record::Type::kRename || type == Record::Type::kReplace;
}

bool HasExpiry(Record::Type type) {
  return type == Record::Type::kSet || type == Record::Type::kExpire;
}

// Decodes the body (type byte plus payload) of a record. `nested` is true for
// the records inside a batch, which may not themselves be batches.
bool DecodeBody(std::string_view body, bool nested, Record* record) {
  if (body.empty()) return false;
  const uint8_t type = static_cast<uint8_t>(body[0]);
  if (type < 1 || type > 18) return false;
  *record = Record();
  record->type = static_cast<Record::Type>(type);
  PayloadReader reader(body.substr(1));

  if (record->type == Record::Type::kBatch) {
    if (nested) return false;
    uint32_t count;
    if (!reader.ReadU32(&count) || count == 0) return false;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t length;
      std::string_view sub_body;
      if (!reader.ReadU32(&length) || !reader.ReadBytes(length, &sub_body)) {
        return false;
      }
      Record sub;
      if (!DecodeBody(sub_body, /*nested=*/true, &sub)) return false;
      record->records.push_back(std::move(sub));
    }
    return reader.done();
  }

  if (!reader.ReadString(&record->key)) return false;

  if (const Shape* shape = ShapeOf(record->type)) {
    if (shape->fresh) {
      uint8_t fresh;
      if (!reader.ReadU8(&fresh)) return false;
      record->n1 = fresh;
    }
    if (shape->strings && !reader.ReadStrings(&record->items)) return false;
    if (shape->ints >= 1 && !reader.ReadI64(&record->n1)) return false;
    if (shape->ints >= 2 && !reader.ReadI64(&record->n2)) return false;
    return reader.done() && FitsShape(*shape, *record);
  }

  if (HasValue(record->type) && !reader.ReadString(&record->value)) {
    return false;
  }
  if (HasExpiry(record->type)) {
    int64_t at;
    if (!reader.ReadI64(&at)) return false;
    if (at >= 0) {
      record->expires_at_ms = at;
    } else if (at != -1 || record->type == Record::Type::kExpire) {
      return false;
    }
  }
  return reader.done();
}

// Returns true if every byte in [offset, offset + n) is zero.
StatusOr<bool> AllZero(File& file, uint64_t offset, uint64_t n) {
  constexpr size_t kChunk = 64 * 1024;
  while (n > 0) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(n, kChunk));
    StatusOr<std::string> chunk = file.Read(offset, want);
    if (!chunk.ok()) return chunk.status();
    if (chunk->size() != want) {
      return Status(StatusCode::kIoError, "short read");
    }
    if (std::any_of(chunk->begin(), chunk->end(),
                    [](char c) { return c != 0; })) {
      return false;
    }
    offset += want;
    n -= want;
  }
  return true;
}

}  // namespace

std::string EncodeHeader() {
  std::string h;
  h.append("QEYDB\0\0\1", 8);
  PutU32(&h, kLogFormatVersion);
  PutU32(&h, 0);  // Reserved.
  return h;
}

Status WriteHeader(File& file) { return file.Append(EncodeHeader()); }

namespace {

// Appends the body (type byte plus payload) of `record` to `body`.
Status EncodeBody(const Record& record, bool nested, std::string* body) {
  if (record.type < Record::Type::kSet || record.type > Record::Type::kSRem) {
    return Status(StatusCode::kInvalidArgument, "unknown record type");
  }
  if (record.expires_at_ms.has_value() &&
      (*record.expires_at_ms < 0 || !HasExpiry(record.type))) {
    return Status(StatusCode::kInvalidArgument, "invalid expiry");
  }
  if (record.type == Record::Type::kExpire &&
      !record.expires_at_ms.has_value()) {
    return Status(StatusCode::kInvalidArgument, "expire record needs expiry");
  }

  body->push_back(static_cast<char>(record.type));
  if (record.type == Record::Type::kBatch) {
    if (nested) {
      return Status(StatusCode::kInvalidArgument, "batches cannot be nested");
    }
    if (record.records.empty() || record.records.size() > UINT32_MAX) {
      return Status(StatusCode::kInvalidArgument, "batch size is invalid");
    }
    PutU32(body, static_cast<uint32_t>(record.records.size()));
    for (const Record& sub : record.records) {
      std::string sub_body;
      Status s = EncodeBody(sub, /*nested=*/true, &sub_body);
      if (!s.ok()) return s;
      PutU32(body, static_cast<uint32_t>(sub_body.size()));
      body->append(sub_body);
    }
  } else if (const Shape* shape = ShapeOf(record.type)) {
    if (!FitsShape(*shape, record)) {
      return Status(StatusCode::kInvalidArgument, "invalid collection record");
    }
    PutString(body, record.key);
    if (shape->fresh) body->push_back(static_cast<char>(record.n1));
    if (shape->strings) PutStrings(body, record.items);
    if (shape->ints >= 1) PutI64(body, record.n1);
    if (shape->ints >= 2) PutI64(body, record.n2);
  } else {
    PutString(body, record.key);
    if (HasValue(record.type)) PutString(body, record.value);
    if (HasExpiry(record.type)) {
      PutI64(body, record.expires_at_ms.value_or(-1));
    }
  }
  if (body->size() > kMaxRecordLength) {
    return Status(StatusCode::kInvalidArgument, "record too large");
  }
  return Status::Ok();
}

}  // namespace

Status EncodeRecord(const Record& record, std::string* out) {
  std::string body;
  Status s = EncodeBody(record, /*nested=*/false, &body);
  if (!s.ok()) return s;

  out->reserve(out->size() + kFrameHeaderSize + body.size());
  PutU32(out, static_cast<uint32_t>(body.size()));
  PutU32(out, Crc32(body));
  out->append(body);
  return Status::Ok();
}

Status AppendRecord(File& file, const Record& record) {
  std::string frame;
  Status s = EncodeRecord(record, &frame);
  if (!s.ok()) return s;
  return file.Append(frame);
}

StatusOr<ReplayResult> Replay(File& file, Recovery recovery,
                              const std::function<void(Record&&)>& apply) {
  const uint64_t size = file.Size();
  ReplayResult result;

  // Reports damage that a crash could have left at the end of the file.
  auto torn_tail = [&](uint64_t valid_size,
                       const char* what) -> StatusOr<ReplayResult> {
    if (recovery == Recovery::kStrict) {
      return Status(StatusCode::kCorruption, what);
    }
    result.valid_size = valid_size;
    result.dropped_bytes = size - valid_size;
    return result;
  };

  if (size == 0) return result;

  // Header.
  const std::string expected_header = EncodeHeader();
  StatusOr<std::string> header = file.Read(0, kLogHeaderSize);
  if (!header.ok()) return header.status();
  if (header->size() < kLogHeaderSize) {
    if (*header != expected_header.substr(0, header->size())) {
      return Status(StatusCode::kCorruption, "not a qey log");
    }
    return torn_tail(0, "truncated header");
  }
  if (header->compare(0, 8, expected_header, 0, 8) != 0) {
    return Status(StatusCode::kCorruption, "bad magic");
  }
  if (GetU32(header->data() + 8) != kLogFormatVersion) {
    return Status(StatusCode::kCorruption, "unsupported format version");
  }

  // Records.
  uint64_t pos = kLogHeaderSize;
  while (pos < size) {
    const uint64_t remaining = size - pos;
    if (remaining < kFrameHeaderSize) {
      return torn_tail(pos, "truncated record header");
    }
    StatusOr<std::string> frame = file.Read(pos, kFrameHeaderSize);
    if (!frame.ok()) return frame.status();
    if (frame->size() != kFrameHeaderSize) {
      return Status(StatusCode::kIoError, "short read");
    }
    const uint32_t length = GetU32(frame->data());
    const uint32_t crc = GetU32(frame->data() + 4);

    if (length == 0 || length > kMaxRecordLength) {
      // A zero-filled tail is what some file systems leave after a crash.
      StatusOr<bool> zeros = AllZero(file, pos, remaining);
      if (!zeros.ok()) return zeros.status();
      if (*zeros) return torn_tail(pos, "zero-filled tail");
      return Status(StatusCode::kCorruption, "invalid record length");
    }
    if (remaining - kFrameHeaderSize < length) {
      return torn_tail(pos, "truncated record");
    }

    StatusOr<std::string> body = file.Read(pos + kFrameHeaderSize, length);
    if (!body.ok()) return body.status();
    if (body->size() != length) {
      return Status(StatusCode::kIoError, "short read");
    }
    const uint64_t end = pos + kFrameHeaderSize + length;
    if (Crc32(*body) != crc) {
      if (end == size) return torn_tail(pos, "checksum mismatch at tail");
      return Status(StatusCode::kCorruption, "checksum mismatch");
    }

    Record record;
    if (!DecodeBody(*body, /*nested=*/false, &record)) {
      return Status(StatusCode::kCorruption, "malformed record");
    }
    apply(std::move(record));
    ++result.records;
    pos = end;
  }

  result.valid_size = size;
  return result;
}

}  // namespace qey
