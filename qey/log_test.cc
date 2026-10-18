#include "qey/log.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
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
Record Append(std::string key, std::string suffix) {
  return Record{Type::kAppend, std::move(key), std::move(suffix), {}};
}
Record Rename(std::string from, std::string to) {
  return Record{Type::kRename, std::move(from), std::move(to), {}};
}
Record HSet(std::string key, std::vector<std::string> items,
            int64_t fresh = 0) {
  Record r;
  r.type = Type::kHSet;
  r.key = std::move(key);
  r.items = std::move(items);
  r.n1 = fresh;
  return r;
}
Record HDel(std::string key, std::vector<std::string> fields) {
  Record r;
  r.type = Type::kHDel;
  r.key = std::move(key);
  r.items = std::move(fields);
  return r;
}
Record Coll(Type type, std::string key, std::vector<std::string> items,
            int64_t n1 = 0, int64_t n2 = 0) {
  Record r;
  r.type = type;
  r.key = std::move(key);
  r.items = std::move(items);
  r.n1 = n1;
  r.n2 = n2;
  return r;
}
Record ZAdd(std::string key, std::vector<std::string> members,
            std::vector<double> scores, int64_t fresh = 0) {
  Record r;
  r.type = Type::kZAdd;
  r.key = std::move(key);
  r.items = std::move(members);
  r.scores = std::move(scores);
  r.n1 = fresh;
  return r;
}
Record Replace(std::string key, std::string value) {
  return Record{Type::kReplace, std::move(key), std::move(value), {}};
}
Record Batch(std::vector<Record> records) {
  Record r;
  r.type = Type::kBatch;
  r.records = std::move(records);
  return r;
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
      Append("a", std::string("\0tail", 5)),
      Rename("b", "c"),
      Append("", ""),
      Batch({Set("x", "1", 5), Del("y"), Rename("x", "z"), Append("z", "!")}),
      Batch({Persist("z")}),
      HSet("h", {"f", "v", "", "", std::string("b\0n", 3), "x"}, 1),
      HSet("h", {"g", "w"}),
      HDel("h", {"f", "nope"}),
      Coll(Type::kRPush, "l", {"a", "", std::string("b\0", 2)}, 1),
      Coll(Type::kLPush, "l", {"z"}),
      Coll(Type::kLPop, "l", {}, 2),
      Coll(Type::kRPop, "l", {}, 1),
      Coll(Type::kLSet, "l", {"new"}, 0),
      Coll(Type::kLTrim, "l", {}, 1, 3),
      Coll(Type::kLTrim, "l", {}, 0, -1),
      Coll(Type::kSAdd, "s", {"m1", "m2", ""}, 1),
      Coll(Type::kSAdd, "s", {"m3"}),
      Coll(Type::kSRem, "s", {"m1", "m3"}),
      ZAdd("z", {"a", "", "b"}, {1.5, -0.25, 1e300}, 1),
      ZAdd("z", {"c"}, {std::numeric_limits<double>::infinity()}),
      ZAdd("z", {"d"}, {-std::numeric_limits<double>::infinity()}),
      ZAdd("z", {"e"}, {std::numeric_limits<double>::denorm_min()}),
      Coll(Type::kZRem, "z", {"a", "c"}),
      Replace("a", "42"),
      Replace("", ""),
      Batch({Replace("k", "1"), Set("j", "2")}),
      Batch({HSet("h2", {"a", "1"}, 1), HDel("h2", {"a"})}),
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
      std::string("\x08\1\0\0\0k", 6),          // kReplace without its value
      std::string("\x7f", 1),                   // an unassigned type
      std::string("\xff", 1),                   // far outside the known types
      std::string("\x05\1\0\0\0k", 6),          // kAppend without its value
      std::string("\x06\1\0\0\0k", 6),          // kRename without its new key
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

TEST(AppendRecordTest, RejectsInvalidBatches) {
  MemFile f;
  EXPECT_EQ(AppendRecord(f, Batch({})).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(AppendRecord(f, Batch({Batch({Del("a")})})).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(AppendRecord(f, Batch({Set("a", "1"), Set("b", "2", -1)})).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(f.Size(), 0);  // nothing was written
}

TEST(ReplayTest, BatchIsAppliedAsOneRecordWithItsMembersInOrder) {
  MemFile f;
  const Record batch = Batch({Set("a", "1"), Del("a"), Set("a", "2")});
  WriteLog(f, {batch});
  Replayed r = ReplayAll(f, Recovery::kStrict);
  ASSERT_TRUE(r.result.ok());
  ASSERT_EQ(r.records.size(), 1);
  EXPECT_EQ(r.records[0], batch);
  EXPECT_EQ(r.result->records, 1);
}

TEST(ReplayTest, TornBatchIsDroppedWholeNotPartly) {
  MemFile f;
  const std::vector<uint64_t> boundaries =
      WriteLog(f, {Set("before", "1"), Batch({Set("a", "1"), Set("b", "2")})});
  // Cut anywhere inside the batch record: none of its members survive.
  for (uint64_t cut = boundaries[1] + 1; cut < boundaries[2]; ++cut) {
    MemFile copy(f.data().substr(0, cut));
    Replayed r = ReplayAll(copy);
    ASSERT_TRUE(r.result.ok()) << "cut=" << cut;
    EXPECT_EQ(r.records, std::vector<Record>{Set("before", "1")})
        << "cut=" << cut;
  }
}

TEST(ReplayTest, ValidChecksumWithMalformedBatchIsCorruption) {
  const std::string del_a("\x02\1\0\0\0a", 6);  // a complete kDel body
  auto u32 = [](uint32_t v) {
    std::string s;
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
    return s;
  };
  const std::string inner = std::string("\x07", 1) + u32(1) + u32(6) + del_a;
  const std::vector<std::string> bad_bodies = {
      std::string("\x07", 1),                                  // no count
      std::string("\x07", 1) + u32(0),                         // empty batch
      std::string("\x07", 1) + u32(2) + u32(6) + del_a,        // count too big
      std::string("\x07", 1) + u32(1) + u32(99) + del_a,       // length too big
      std::string("\x07", 1) + u32(1) + u32(6) + del_a + "!",  // trailing byte
      std::string("\x07", 1) + u32(1) + u32(0),                // empty member
      std::string("\x07", 1) + u32(1) + u32(3) + std::string("\x63\0\0", 3),
      // A batch inside a batch, otherwise well formed.
      std::string("\x07", 1) + u32(1) + u32(inner.size()) + inner,
  };
  for (const std::string& body : bad_bodies) {
    MemFile f(HeaderBytes() + Frame(body));
    Replayed r = ReplayAll(f);
    ASSERT_FALSE(r.result.ok());
    EXPECT_EQ(r.result.status().code(), StatusCode::kCorruption);
  }

  // Each piece of the nested case is valid on its own, so it is the nesting
  // that is rejected.
  MemFile inner_alone(HeaderBytes() + Frame(inner));
  EXPECT_TRUE(ReplayAll(inner_alone, Recovery::kStrict).result.ok());

  // The well-formed version of the same shape is accepted.
  MemFile ok(HeaderBytes() +
             Frame(std::string("\x07", 1) + u32(1) + u32(6) + del_a));
  EXPECT_TRUE(ReplayAll(ok, Recovery::kStrict).result.ok());
}

TEST(AppendRecordTest, RejectsInvalidHashRecords) {
  MemFile f;
  EXPECT_EQ(AppendRecord(f, HSet("h", {})).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(AppendRecord(f, HSet("h", {"field-without-value"})).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(AppendRecord(f, HSet("h", {"f", "v"}, 2)).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(AppendRecord(f, HDel("h", {})).code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(f.Size(), 0);
}

TEST(AppendRecordTest, RejectsInvalidListRecords) {
  MemFile f;
  for (const Record& bad : {
           Coll(Type::kLPush, "l", {}),            // nothing to push
           Coll(Type::kRPush, "l", {"a"}, 2),      // bad fresh flag
           Coll(Type::kLPop, "l", {}, 0),          // must pop at least 1
           Coll(Type::kRPop, "l", {"x"}, 1),       // takes no strings
           Coll(Type::kLSet, "l", {"a", "b"}, 0),  // exactly one value
           Coll(Type::kLSet, "l", {"a"}, -1),      // negative index
           Coll(Type::kLTrim, "l", {}, -1, 5),     // negative start
           Coll(Type::kLTrim, "l", {}, 0, -2),     // stop below -1
           Coll(Type::kLTrim, "l", {"x"}, 0, 1),   // takes no strings
           Coll(Type::kSAdd, "s", {}),             // nothing to add
           Coll(Type::kSAdd, "s", {"a"}, 2),       // bad fresh flag
           Coll(Type::kSRem, "s", {}),             // nothing to remove
           Coll(Type::kSRem, "s", {"a"}, 1),       // no flag in this one
       }) {
    EXPECT_EQ(AppendRecord(f, bad).code(), StatusCode::kInvalidArgument);
  }
  EXPECT_EQ(f.Size(), 0);
}

TEST(ReplayTest, ValidChecksumWithMalformedListRecordIsCorruption) {
  auto u32 = [](uint32_t v) {
    std::string s;
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
    return s;
  };
  auto i64 = [](int64_t v) {
    std::string s;
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
    return s;
  };
  const std::string key = u32(1) + "l";
  const std::string one = u32(1) + "a";
  const std::vector<std::string> bad_bodies = {
      std::string("\x0b", 1) + key + std::string("\x00", 1) + u32(0),
      std::string("\x0c", 1) + key + std::string("\x05", 1) + u32(1) + one,
      std::string("\x0d", 1) + key + i64(0),                       // pop 0
      std::string("\x0d", 1) + key,                                // no count
      std::string("\x0e", 1) + key + i64(1) + "!",                 // trailing
      std::string("\x0f", 1) + key + u32(1) + one + i64(-1),       // index -1
      std::string("\x0f", 1) + key + u32(2) + one + one + i64(0),  // 2 values
      std::string("\x10", 1) + key + i64(0),                       // no stop
      std::string("\x10", 1) + key + i64(0) + i64(-2),             // stop < -1
  };
  for (const std::string& body : bad_bodies) {
    MemFile f(HeaderBytes() + Frame(body));
    Replayed r = ReplayAll(f);
    ASSERT_FALSE(r.result.ok());
    EXPECT_EQ(r.result.status().code(), StatusCode::kCorruption);
  }
  MemFile ok(HeaderBytes() +
             Frame(std::string("\x0b", 1) + key + std::string("\x01", 1) +
                   u32(1) + one) +
             Frame(std::string("\x0d", 1) + key + i64(1)) +
             Frame(std::string("\x0f", 1) + key + u32(1) + one + i64(0)) +
             Frame(std::string("\x10", 1) + key + i64(0) + i64(-1)));
  EXPECT_TRUE(ReplayAll(ok, Recovery::kStrict).result.ok());
}

TEST(AppendRecordTest, RejectsInvalidSortedSetRecords) {
  MemFile f;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const Record& bad : {
           ZAdd("z", {}, {}),           // nothing to add
           ZAdd("z", {"a"}, {}),        // a member with no score
           ZAdd("z", {"a"}, {1, 2}),    // a score with no member
           ZAdd("z", {"a"}, {nan}),     // NaN is not a score
           ZAdd("z", {"a"}, {1}, 2),    // bad fresh flag
           Coll(Type::kZRem, "z", {}),  // nothing to remove
       }) {
    EXPECT_EQ(AppendRecord(f, bad).code(), StatusCode::kInvalidArgument);
  }
  Record stray = Coll(Type::kHDel, "h", {"f"});
  stray.scores = {1.0};  // scores belong only to ZADD
  EXPECT_EQ(AppendRecord(f, stray).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(f.Size(), 0);
}

TEST(ReplayTest, ScoresRoundTripExactly) {
  MemFile f;
  const std::vector<double> scores = {0.0,
                                      -0.0,
                                      0.1,
                                      1.0 / 3.0,
                                      std::numeric_limits<double>::max(),
                                      std::numeric_limits<double>::lowest(),
                                      std::numeric_limits<double>::min(),
                                      std::numeric_limits<double>::denorm_min(),
                                      std::numeric_limits<double>::infinity(),
                                      -std::numeric_limits<double>::infinity()};
  std::vector<std::string> members;
  for (size_t i = 0; i < scores.size(); ++i)
    members.push_back("m" + std::to_string(i));
  WriteLog(f, {ZAdd("z", members, scores, 1)});
  Replayed r = ReplayAll(f, Recovery::kStrict);
  ASSERT_TRUE(r.result.ok());
  ASSERT_EQ(r.records.size(), 1);
  ASSERT_EQ(r.records[0].scores.size(), scores.size());
  for (size_t i = 0; i < scores.size(); ++i) {
    // Compare the bits, so that -0.0 and 0.0 are told apart.
    EXPECT_EQ(std::memcmp(&r.records[0].scores[i], &scores[i], sizeof(double)),
              0)
        << i;
  }
}

TEST(ReplayTest, ValidChecksumWithMalformedSortedSetRecordIsCorruption) {
  auto u32 = [](uint32_t v) {
    std::string s;
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
    return s;
  };
  auto f64 = [](double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    std::string s;
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>(bits >> (8 * i)));
    return s;
  };
  const std::string key = u32(1) + "z";
  const std::string one = u32(1) + "a";
  const std::vector<std::string> bad_bodies = {
      std::string("\x13", 1) + key + std::string("\x00", 1) + u32(1) +
          one,  // no score
      std::string("\x13", 1) + key + std::string("\x00", 1) + u32(1) + one +
          f64(std::numeric_limits<double>::quiet_NaN()),
      std::string("\x13", 1) + key + std::string("\x00", 1) + u32(1) + one +
          f64(1) + f64(2),  // extra score
      std::string("\x13", 1) + key + std::string("\x00", 1) +
          u32(0),                             // no members
      std::string("\x14", 1) + key + u32(0),  // remove nothing
  };
  for (const std::string& body : bad_bodies) {
    MemFile f(HeaderBytes() + Frame(body));
    Replayed r = ReplayAll(f);
    ASSERT_FALSE(r.result.ok());
    EXPECT_EQ(r.result.status().code(), StatusCode::kCorruption);
  }
  MemFile ok(HeaderBytes() +
             Frame(std::string("\x13", 1) + key + std::string("\x01", 1) +
                   u32(1) + one + f64(2.5)) +
             Frame(std::string("\x14", 1) + key + u32(1) + one));
  EXPECT_TRUE(ReplayAll(ok, Recovery::kStrict).result.ok());
}

TEST(ReplayTest, ValidChecksumWithMalformedHashRecordIsCorruption) {
  auto u32 = [](uint32_t v) {
    std::string s;
    for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i)));
    return s;
  };
  const std::string key = u32(1) + "h";
  const std::string one = u32(1) + "a";  // the string "a"
  const std::vector<std::string> bad_bodies = {
      std::string("\x09", 1) + key,                           // no rest
      std::string("\x09", 1) + key + std::string("\x00", 1),  // no items
      std::string("\x09", 1) + key + std::string("\x02", 1) + u32(2) + one +
          one,
      std::string("\x09", 1) + key + std::string("\x00", 1) + u32(1) + one,
      std::string("\x09", 1) + key + std::string("\x00", 1) + u32(0),
      std::string("\x09", 1) + key + std::string("\x00", 1) + u32(9999) + one,
      std::string("\x09", 1) + key + std::string("\x00", 1) + u32(2) + one +
          one + "!",
      std::string("\x0a", 1) + key,  // no items
      std::string("\x0a", 1) + key + u32(0),
      std::string("\x0a", 1) + key + u32(1) + one + "!",
  };
  for (const std::string& body : bad_bodies) {
    MemFile f(HeaderBytes() + Frame(body));
    Replayed r = ReplayAll(f);
    ASSERT_FALSE(r.result.ok());
    EXPECT_EQ(r.result.status().code(), StatusCode::kCorruption);
  }
  // The well-formed forms are accepted.
  MemFile ok(HeaderBytes() +
             Frame(std::string("\x09", 1) + key + std::string("\x01", 1) +
                   u32(2) + one + one) +
             Frame(std::string("\x0a", 1) + key + u32(1) + one));
  EXPECT_TRUE(ReplayAll(ok, Recovery::kStrict).result.ok());
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
