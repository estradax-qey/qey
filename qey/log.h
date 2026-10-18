#ifndef QEY_LOG_H_
#define QEY_LOG_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "qey/file.h"
#include "qey/status.h"

namespace qey {

// The on-disk log format is described in docs/design/persistence.md.

// How Replay() treats a damaged tail of the log.
enum class Recovery {
  // Stop at the damage and report the bytes to discard.
  kTruncate,
  // Treat any damage as an error.
  kStrict,
};

// One logged mutation.
struct Record {
  enum class Type : uint8_t {
    kSet = 1,      // key, value, expires_at_ms (optional)
    kDel = 2,      // key
    kExpire = 3,   // key, expires_at_ms (required)
    kPersist = 4,  // key
    kAppend = 5,   // key, value (the bytes to append)
    kRename = 6,   // key (the source), value (the new key)
    // `records`: a group of changes that is applied entirely or not at all.
    // They share one checksum, so a crash cannot leave part of them in the
    // log. May not be nested, and may not be empty.
    kBatch = 7,
    // key, value: replaces the value and keeps the key's TTL, creating the key
    // without one if it is missing. For changes such as Incr() that must not
    // touch the expiry.
    kReplace = 8,
    // Hashes. In records that create a collection, `n1` is 1 if the key did
    // not exist when the change was made: the key then starts as an empty
    // collection with no TTL, replacing whatever the log still holds under
    // that name (which may be a stale entry that expired unnoticed).
    kHSet = 9,   // key, n1 (fresh), items (field, value, field, value, ...)
    kHDel = 10,  // key, items (the fields to remove)
  };

  Record() = default;
  Record(Type type, std::string key, std::string value,
         std::optional<int64_t> expires_at_ms)
      : type(type),
        key(std::move(key)),
        value(std::move(value)),
        expires_at_ms(expires_at_ms) {}

  Type type = Type::kSet;
  std::string key;
  std::string value;  // kSet, kAppend, kRename and kReplace only.
  // Absolute expiry as Unix milliseconds (>= 0). Used by kSet (nullopt means
  // no expiry) and kExpire (required).
  std::optional<int64_t> expires_at_ms;
  std::vector<Record> records;  // kBatch only.
  // The strings of a collection change: fields and values, members, or
  // elements, depending on the type.
  std::vector<std::string> items;
  // Small integer argument. Its meaning depends on the type (see above).
  int64_t n1 = 0;

  bool operator==(const Record&) const = default;
};

inline constexpr size_t kLogHeaderSize = 16;
inline constexpr uint32_t kLogFormatVersion = 1;
// Upper bound on a record's body (type byte plus payload).
inline constexpr uint32_t kMaxRecordLength = 512u << 20;

// Returns the bytes of the log header.
std::string EncodeHeader();

// Appends the encoded form of `record` to `out`. Fails with kInvalidArgument,
// leaving `out` unchanged, if the record is malformed or too large.
Status EncodeRecord(const Record& record, std::string* out);

// Appends the log header. Call once on an empty file.
Status WriteHeader(File& file);

// Appends `record` with a single File::Append call. Fails with
// kInvalidArgument if the record is malformed or too large, without writing.
Status AppendRecord(File& file, const Record& record);

struct ReplayResult {
  // Length of the valid prefix: header plus all intact records. 0 for an empty
  // file or a torn header, in which case the caller must (re)write the
  // header. Callers should truncate the file to this length if
  // `dropped_bytes` is non-zero.
  uint64_t valid_size = 0;
  // Bytes after `valid_size` that were discarded as a damaged tail.
  uint64_t dropped_bytes = 0;
  // Number of records passed to the callback.
  size_t records = 0;
};

// Reads the log from the start and calls `apply` for each intact record, in
// order. Does not modify the file.
//
// Damage is classified as a torn tail only if it is where a crash during an
// append could leave it: a header or record cut short by the end of the file,
// a record whose last bytes fail the checksum, or a zero-filled tail. Anything
// else, such as a checksum failure with more data after the record, is
// kCorruption. A damaged length field that points past the end of the file
// cannot be told apart from a cut-short record, so it is treated as a torn
// tail; `dropped_bytes` reports how much was discarded.
//
// With Recovery::kStrict a torn tail is also kCorruption.
StatusOr<ReplayResult> Replay(File& file, Recovery recovery,
                              const std::function<void(Record&&)>& apply);

}  // namespace qey

#endif  // QEY_LOG_H_
