#include "qey/log.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "qey/crc32.h"
#include "qey/memory_file.h"

namespace qey {
namespace {

using Type = Record::Type;

Record Set(std::string key, std::string value,
           std::optional<int64_t> expires_at_ms = std::nullopt) {
  return Record{Type::kSet, std::move(key), std::move(value), expires_at_ms};
}
Record Del(std::string key) {
  return Record{Type::kDel, std::move(key), "", {}};
}
Record Expire(std::string key, int64_t at) {
  return Record{Type::kExpire, std::move(key), "", at};
}
Record Persist(std::string key) {
  return Record{Type::kPersist, std::move(key), "", {}};
}

std::vector<Record> SampleRecords() {
  return {
      Set("a", "1"),
      Set("b", "two", 1700000000000),
      Set("", ""),
      Set(std::string("bin\0key", 7), std::string("\0\xff\1", 3), 0),
      Expire("a", 1800000000000),
      Persist("b"),
      Del("a"),
  };
}

// Writes a log of `records` to `file` and returns the byte offset at which
// each record ends (preceded by the header's end).
std::vector<uint64_t> WriteLog(MemFile& file,
                               const std::vector<Record>& records) {
  EXPECT_TRUE(WriteHeader(file).ok());
  std::vector<uint64_t> boundaries = {file.Size()};
  for (const Record& r : records) {
    EXPECT_TRUE(AppendRecord(file, r).ok());
    boundaries.push_back(file.Size());
  }
  return boundaries;
}

struct Replayed {
  StatusOr<ReplayResult> result;
  std::vector<Record> records;
};

Replayed ReplayAll(MemFile& file, Recovery recovery = Recovery::kTruncate) {
  std::vector<Record> records;
  StatusOr<ReplayResult> result = Replay(
      file, recovery, [&](Record&& r) { records.push_back(std::move(r)); });
  return {std::move(result), std::move(records)};
}

void PutU32(std::string* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>(v >> (8 * i)));
}

// Frames `body` (type byte plus payload) with a valid length and checksum.
std::string Frame(std::string_view body) {
  std::string out;
  PutU32(&out, static_cast<uint32_t>(body.size()));
  PutU32(&out, Crc32(body));
  out.append(body);
  return out;
}

std::string HeaderBytes() {
  MemFile f;
  EXPECT_TRUE(WriteHeader(f).ok());
  return f.data();
}

TEST(LogTest, HeaderIsSixteenBytes) {
  MemFile f;
  ASSERT_TRUE(WriteHeader(f).ok());
  EXPECT_EQ(f.Size(), kLogHeaderSize);
  EXPECT_EQ(f.data().substr(0, 5), "QEYDB");
}

TEST(ReplayTest, EmptyFileHasNoRecords) {
  MemFile f;
  Replayed r = ReplayAll(f);
  ASSERT_TRUE(r.result.ok());
  EXPECT_EQ(r.result->valid_size, 0);
  EXPECT_EQ(r.result->dropped_bytes, 0);
  EXPECT_EQ(r.result->records, 0);
}

TEST(ReplayTest, HeaderOnly) {
  MemFile f;
  ASSERT_TRUE(WriteHeader(f).ok());
  Replayed r = ReplayAll(f, Recovery::kStrict);
  ASSERT_TRUE(r.result.ok());
  EXPECT_EQ(r.result->valid_size, kLogHeaderSize);
  EXPECT_EQ(r.result->records, 0);
}

TEST(ReplayTest, RoundTripsEveryRecordType) {
  MemFile f;
  const std::vector<Record> records = SampleRecords();
  WriteLog(f, records);
  Replayed r = ReplayAll(f, Recovery::kStrict);
  ASSERT_TRUE(r.result.ok());
  EXPECT_EQ(r.records, records);
  EXPECT_EQ(r.result->valid_size, f.Size());
  EXPECT_EQ(r.result->dropped_bytes, 0);
  EXPECT_EQ(r.result->records, records.size());
}

TEST(ReplayTest, RecoversPrefixWhenTruncatedAtEveryOffset) {
  MemFile full;
  const std::vector<Record> records = SampleRecords();
  const std::vector<uint64_t> boundaries = WriteLog(full, records);

  for (uint64_t cut = 0; cut <= full.Size(); ++cut) {
    MemFile f(full.data().substr(0, cut));
    Replayed r = ReplayAll(f);
    ASSERT_TRUE(r.result.ok()) << "cut=" << cut;

    // The valid prefix ends at the last boundary at or before `cut`.
    uint64_t valid = 0;
    size_t whole = 0;
    for (size_t i = 0; i < boundaries.size(); ++i) {
      if (boundaries[i] <= cut) {
        valid = boundaries[i];
        whole = i;
      }
    }
    EXPECT_EQ(r.result->valid_size, valid) << "cut=" << cut;
    EXPECT_EQ(r.result->dropped_bytes, cut - valid) << "cut=" << cut;
    EXPECT_EQ(r.records,
              std::vector<Record>(records.begin(), records.begin() + whole))
        << "cut=" << cut;
  }
}

TEST(ReplayTest, StrictAcceptsOnlyRecordBoundaries) {
  MemFile full;
  const std::vector<uint64_t> boundaries = WriteLog(full, SampleRecords());

  for (uint64_t cut = 0; cut <= full.Size(); ++cut) {
    MemFile f(full.data().substr(0, cut));
    Replayed r = ReplayAll(f, Recovery::kStrict);
    const bool at_boundary =
        cut == 0 || std::find(boundaries.begin(), boundaries.end(), cut) !=
                        boundaries.end();
    if (at_boundary) {
      EXPECT_TRUE(r.result.ok()) << "cut=" << cut;
    } else {
      ASSERT_FALSE(r.result.ok()) << "cut=" << cut;
      EXPECT_EQ(r.result.status().code(), StatusCode::kCorruption);
    }
  }
}

TEST(ReplayTest, ZeroFilledTailIsDropped) {
  MemFile f;
  const std::vector<Record> records = SampleRecords();
  const std::vector<uint64_t> boundaries = WriteLog(f, records);
  f.data().append(std::string(37, '\0'));

  Replayed r = ReplayAll(f);
  ASSERT_TRUE(r.result.ok());
  EXPECT_EQ(r.records, records);
  EXPECT_EQ(r.result->valid_size, boundaries.back());
  EXPECT_EQ(r.result->dropped_bytes, 37);

  MemFile strict(f.data());
  EXPECT_EQ(ReplayAll(strict, Recovery::kStrict).result.status().code(),
            StatusCode::kCorruption);
}

TEST(ReplayTest, ChecksumFailureInLastRecordIsDropped) {
  MemFile f;
  const std::vector<Record> records = SampleRecords();
  const std::vector<uint64_t> boundaries = WriteLog(f, records);
  f.data()[f.Size() - 1] ^= 0x01;

  Replayed r = ReplayAll(f);
  ASSERT_TRUE(r.result.ok());
  EXPECT_EQ(r.records, std::vector<Record>(records.begin(), records.end() - 1));
  EXPECT_EQ(r.result->valid_size, boundaries[boundaries.size() - 2]);

  MemFile strict(f.data());
  EXPECT_EQ(ReplayAll(strict, Recovery::kStrict).result.status().code(),
            StatusCode::kCorruption);
}

TEST(ReplayTest, ChecksumFailureBeforeMoreDataIsCorruption) {
  MemFile f;
  const std::vector<uint64_t> boundaries = WriteLog(f, SampleRecords());
  // Flip a bit inside the second record's body.
  f.data()[boundaries[1] + 10] ^= 0x01;

  for (Recovery mode : {Recovery::kTruncate, Recovery::kStrict}) {
    MemFile copy(f.data());
    Replayed r = ReplayAll(copy, mode);
    ASSERT_FALSE(r.result.ok());
    EXPECT_EQ(r.result.status().code(), StatusCode::kCorruption);
  }
}

TEST(ReplayTest, InvalidLengthWithNonZeroDataIsCorruption) {
  MemFile f;
  WriteLog(f, {Set("a", "1")});
  std::string junk;
  PutU32(&junk, 0);  // length 0 is never valid
  PutU32(&junk, 0xDEADBEEF);
  f.data().append(junk);
  EXPECT_EQ(ReplayAll(f).result.status().code(), StatusCode::kCorruption);
}

TEST(ReplayTest, RejectsBadMagicAndVersion) {
  std::string bad_magic = HeaderBytes();
  bad_magic[0] = 'X';
  MemFile a(bad_magic);
  EXPECT_EQ(ReplayAll(a).result.status().code(), StatusCode::kCorruption);

  std::string bad_version = HeaderBytes();
  bad_version[8] = 2;
  MemFile b(bad_version);
  EXPECT_EQ(ReplayAll(b).result.status().code(), StatusCode::kCorruption);

  MemFile c("garbage that is not a log");
  EXPECT_EQ(ReplayAll(c).result.status().code(), StatusCode::kCorruption);

  MemFile d("QX");  // too short, and not a prefix of the header
  EXPECT_EQ(ReplayAll(d).result.status().code(), StatusCode::kCorruption);
}

TEST(ReplayTest, ValidChecksumWithMalformedBodyIsCorruption) {
  const std::vector<std::string> bad_bodies = {
      std::string("\x01", 1),                   // kSet with no fields
      std::string("\x63", 1),                   // unknown type 99
      std::string("\x02\1\0\0\0k!", 7),         // kDel with trailing byte
      std::string("\x02\xff\xff\xff\xffk", 6),  // string longer than body
      std::string("\x03\1\0\0\0k", 6) +
          std::string("\xfe\xff\xff\xff\xff\xff\xff\xff", 8),  // expiry -2
      std::string("\x03\1\0\0\0k", 6) +
          std::string("\xff\xff\xff\xff\xff\xff\xff\xff",
                      8),  // expire w/o time
  };
  for (const std::string& body : bad_bodies) {
    MemFile f(HeaderBytes() + Frame(body));
    Replayed r = ReplayAll(f);
    ASSERT_FALSE(r.result.ok());
    EXPECT_EQ(r.result.status().code(), StatusCode::kCorruption);
  }
}

TEST(AppendRecordTest, RejectsInvalidRecords) {
  MemFile f;
  EXPECT_EQ(AppendRecord(f, Set("k", "v", -5)).code(),
            StatusCode::kInvalidArgument);
  Record no_expiry = Expire("k", 1);
  no_expiry.expires_at_ms.reset();
  EXPECT_EQ(AppendRecord(f, no_expiry).code(), StatusCode::kInvalidArgument);
  Record del_with_expiry = Del("k");
  del_with_expiry.expires_at_ms = 5;
  EXPECT_EQ(AppendRecord(f, del_with_expiry).code(),
            StatusCode::kInvalidArgument);
  Record bad_type = Del("k");
  bad_type.type = static_cast<Type>(42);
  EXPECT_EQ(AppendRecord(f, bad_type).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(f.Size(), 0);  // nothing was written
}

TEST(AppendRecordTest, FailedAppendLeavesRecoverableTail) {
  MemFile f;
  const std::vector<Record> records = SampleRecords();
  const std::vector<uint64_t> boundaries = WriteLog(f, records);

  // Fail partway through the next record, like a crash mid-write.
  f.FailAppendsBeyond(boundaries.back() + 5);
  EXPECT_EQ(AppendRecord(f, Set("late", "write")).code(), StatusCode::kIoError);
  EXPECT_EQ(f.Size(), boundaries.back() + 5);

  Replayed r = ReplayAll(f);
  ASSERT_TRUE(r.result.ok());
  EXPECT_EQ(r.records, records);
  EXPECT_EQ(r.result->valid_size, boundaries.back());
  EXPECT_EQ(r.result->dropped_bytes, 5);
}

}  // namespace
}  // namespace qey
