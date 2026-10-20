#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "qey/db.h"
#include "qey/log.h"
#include "qey/memory_file.h"

namespace qey {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

// Opens databases over a MemFile, with fake clocks that tests control.
class PersistTest : public testing::Test {
 protected:
  OpenOptions Options(SyncPolicy sync = SyncPolicy::kAlways,
                      Recovery recovery = Recovery::kTruncate) {
    OpenOptions o;
    o.sync = sync;
    o.recovery = recovery;
    o.now = [this] { return steady_; };
    o.wall_now = [this] { return wall_; };
    return o;
  }

  StatusOr<std::unique_ptr<Db>> TryOpen(OpenOptions o) {
    auto storage = std::make_unique<MemStorage>(&file_);
    storage_ = storage.get();
    return Db::Open(std::move(storage), std::move(o));
  }

  std::unique_ptr<Db> Open(OpenOptions o) {
    StatusOr<std::unique_ptr<Db>> r = TryOpen(std::move(o));
    EXPECT_TRUE(r.ok()) << r.status().message();
    return r.ok() ? *std::move(r) : nullptr;
  }
  std::unique_ptr<Db> Open() { return Open(Options()); }

  void Advance(milliseconds d) {
    steady_ += d;
    wall_ += d;
  }

  // Simulates the process exiting and a new one starting after `downtime`:
  // the monotonic clock starts over, wall-clock time keeps going.
  void Restart(milliseconds downtime) {
    steady_ = Db::Clock::time_point(milliseconds(5));
    wall_ += downtime;
  }

  Db::Clock::time_point steady_ = Db::Clock::time_point(seconds(1000));
  std::chrono::system_clock::time_point wall_ =
      std::chrono::system_clock::time_point(seconds(1700000000));
  MemFile file_;
  MemStorage* storage_ = nullptr;  // Valid while the Db from TryOpen lives.
};

TEST_F(PersistTest, NewFileGetsHeaderAndSync) {
  auto db = Open();
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(file_.Size(), kLogHeaderSize);
  EXPECT_EQ(file_.sync_count(), 1);
  EXPECT_EQ(db->recovery_info().records_replayed, 0);
  EXPECT_EQ(db->recovery_info().dropped_bytes, 0);
}

TEST_F(PersistTest, ValuesSurviveReopen) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("a", "1").ok());
    ASSERT_TRUE(db->Set("b", std::string("x\0y", 3)).ok());
    ASSERT_TRUE(db->Set("a", "2").ok());
    ASSERT_TRUE(db->Set("gone", "x").ok());
    ASSERT_TRUE(*db->Del("gone"));
  }
  auto db = Open();
  EXPECT_EQ(db->Get("a"), "2");
  EXPECT_EQ(db->Get("b"), std::string("x\0y", 3));
  EXPECT_EQ(db->Get("gone"), std::nullopt);
  EXPECT_EQ(db->Size(), 2);
  EXPECT_EQ(db->recovery_info().records_replayed, 5);
}

TEST_F(PersistTest, IncrSurvivesReopen) {
  {
    auto db = Open();
    ASSERT_EQ(*db->Incr("n", 5), 5);
    ASSERT_EQ(*db->Decr("n", 2), 3);
  }
  auto db = Open();
  EXPECT_EQ(db->Get("n"), "3");
  EXPECT_EQ(*db->Incr("n"), 4);
}

TEST_F(PersistTest, TtlCountsDownAcrossRestart) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("k", "v", seconds(10)).ok());
  }
  Restart(seconds(4));
  auto db = Open();
  Db::TtlInfo ttl = db->GetTtl("k");
  EXPECT_EQ(ttl.state, Db::TtlState::kExpiring);
  EXPECT_EQ(ttl.remaining, seconds(6));
  EXPECT_EQ(db->Get("k"), "v");

  Advance(seconds(6));
  EXPECT_EQ(db->Get("k"), std::nullopt);
}

TEST_F(PersistTest, KeysExpiredWhileClosedAreDroppedOnOpen) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("short", "v", seconds(10)).ok());
    ASSERT_TRUE(db->Set("long", "v", seconds(100)).ok());
    ASSERT_TRUE(db->Set("forever", "v").ok());
  }
  Restart(seconds(50));
  auto db = Open();
  EXPECT_EQ(db->Size(), 2);
  EXPECT_FALSE(db->Exists("short"));
  EXPECT_TRUE(db->Exists("long"));
  EXPECT_TRUE(db->Exists("forever"));
}

TEST_F(PersistTest, AppendRenameAndClearSurviveReopen) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("a", "1").ok());
    ASSERT_EQ(*db->Append("a", "23"), 3);
    ASSERT_EQ(*db->Append("fresh", "x"), 1);
    ASSERT_TRUE(db->Set("b", "vb", seconds(100)).ok());
    ASSERT_TRUE(*db->Rename("b", "c"));
    ASSERT_TRUE(db->Set("gone", "x").ok());
    ASSERT_TRUE(db->Clear().ok());
    ASSERT_TRUE(db->Set("e", "after clear").ok());
    ASSERT_EQ(*db->Append("e", "!"), 12);
  }
  auto db = Open();
  EXPECT_EQ(db->Keys(), std::vector<std::string>{"e"});
  EXPECT_EQ(db->Get("e"), "after clear!");
}

TEST_F(PersistTest, AppendedAndRenamedValuesAndTtlsSurviveRestart) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("a", "1").ok());
    ASSERT_EQ(*db->Append("a", "23"), 3);
    ASSERT_TRUE(db->Set("b", "vb", seconds(100)).ok());
    ASSERT_EQ(*db->Append("b", "!"), 3);
    ASSERT_TRUE(*db->Rename("b", "c"));
    ASSERT_TRUE(db->Set("d", "old", seconds(5)).ok());
    ASSERT_TRUE(*db->Rename("a", "d"));  // replaces d and its TTL
  }
  Restart(seconds(10));
  auto db = Open();
  EXPECT_EQ(db->Keys(), (std::vector<std::string>{"c", "d"}));
  EXPECT_EQ(db->Get("c"), "vb!");
  EXPECT_EQ(db->GetTtl("c").remaining, seconds(90));
  EXPECT_EQ(db->Get("d"), "123");
  EXPECT_EQ(db->GetTtl("d").state, Db::TtlState::kNoExpiry);
}

TEST_F(PersistTest, AppendLogsOnlyTheSuffix) {
  auto db = Open();
  ASSERT_TRUE(db->Set("k", std::string(10000, 'x')).ok());
  const uint64_t before = file_.Size();
  ASSERT_EQ(*db->Append("k", "y"), 10001);
  // frame (8) + type (1) + key (4 + 1) + suffix (4 + 1)
  EXPECT_EQ(file_.Size() - before, 19);
}

TEST_F(PersistTest, AppendToAnExpiredKeyDoesNotResurrectItAfterRestart) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("k", "old", seconds(10)).ok());
    Advance(seconds(10));  // expired, but still in memory
    ASSERT_EQ(*db->Append("k", "new"), 3);
  }
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(db->Get("k"), "new");  // not "oldnew"
  EXPECT_EQ(db->GetTtl("k").state, Db::TtlState::kNoExpiry);
}

TEST_F(PersistTest, RenameIsOneRecordSoATornWriteLeavesItUndone) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("a", "va").ok());
    ASSERT_TRUE(db->Set("b", "vb", seconds(100)).ok());
  }
  const std::string before = file_.data();
  {
    auto db = Open();
    ASSERT_TRUE(*db->Rename("a", "b"));
  }
  // frame (8) + type (1) + from (4 + 1) + to (4 + 1)
  ASSERT_EQ(file_.Size() - before.size(), 19);

  // A complete record: renamed, and "b" lost its TTL.
  {
    MemFile copy(file_.data());
    auto db = Db::Open(std::make_unique<MemStorage>(&copy), Options());
    ASSERT_TRUE(db.ok());
    EXPECT_FALSE((*db)->Exists("a"));
    EXPECT_EQ((*db)->Get("b"), "va");
  }
  // One byte short of complete: as if the process died mid-write.
  file_.data().pop_back();
  auto db = Open();
  EXPECT_EQ(db->recovery_info().dropped_bytes, 18);
  EXPECT_EQ(db->Get("a"), "va");
  EXPECT_EQ(db->Get("b"), "vb");
  EXPECT_EQ(db->GetTtl("b").state, Db::TtlState::kExpiring);
}

TEST_F(PersistTest, ClearEmptiesTheLogAtOnce) {
  auto db = Open();
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Set("k" + std::to_string(i), "value").ok());
  }
  ASSERT_GT(file_.Size(), 1000);
  ASSERT_TRUE(db->Clear().ok());
  EXPECT_EQ(file_.Size(), kLogHeaderSize);
  EXPECT_EQ(storage_->replace_count(), 1);
  EXPECT_EQ(db->Size(), 0);

  ASSERT_TRUE(db->Set("after", "1").ok());
  db.reset();
  db = Open();
  EXPECT_EQ(db->Keys(), std::vector<std::string>{"after"});
  EXPECT_EQ(db->recovery_info().records_replayed, 1);
}

TEST_F(PersistTest, FailedClearChangesNothing) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1").ok());
  ASSERT_TRUE(db->Set("b", "2", seconds(10)).ok());
  const std::string before = file_.data();

  storage_->FailReplaceBegin(true);
  EXPECT_EQ(db->Clear().code(), StatusCode::kIoError);
  storage_->FailReplaceBegin(false);
  storage_->FailReplaceCommit(true);
  EXPECT_EQ(db->Clear().code(), StatusCode::kIoError);

  EXPECT_EQ(file_.data(), before);  // the log is untouched...
  EXPECT_EQ(db->Keys(),
            (std::vector<std::string>{"a", "b"}));  // ...and so is memory
  EXPECT_EQ(db->GetTtl("b").remaining, seconds(10));
  ASSERT_TRUE(db->Set("c", "3").ok());  // and the Db still works

  storage_->FailReplaceCommit(false);
  ASSERT_TRUE(db->Clear().ok());
  EXPECT_EQ(db->Size(), 0);
}

TEST_F(PersistTest, RejectedWritesLogNothing) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1").ok());
  const uint64_t size = file_.Size();
  const std::string big_key(Db::kMaxKeySize + 1, 'k');
  EXPECT_FALSE(db->Set(big_key, "v").ok());
  EXPECT_FALSE(db->Append(big_key, "v").ok());
  EXPECT_FALSE(db->Rename("a", big_key).ok());
  EXPECT_FALSE(*db->Rename("missing", "x"));
  EXPECT_TRUE(*db->Rename("a", "a"));  // renaming onto itself is a no-op
  EXPECT_EQ(file_.Size(), size);
}

std::vector<std::string> KeysOf(Db& db) { return db.Keys(); }

// A readable description of what `key` holds, to compare two Dbs.
std::string Dump(Db& db, const std::string& key) {
  switch (db.Type(key)) {
    case KeyType::kNone:
      return "none";
    case KeyType::kString:
      return "string:" + *db.Get(key);
    case KeyType::kHash: {
      std::string out = "hash:";
      // Hold the result: iterating `*db.HGetAll(key)` directly would loop over
      // a vector inside a temporary that is already gone.
      StatusOr<std::vector<FieldValue>> all = db.HGetAll(key);
      for (const auto& [field, value] : *all) out += field + "=" + value + ";";
      return out;
    }
    case KeyType::kList: {
      std::string out = "list:";
      StatusOr<std::vector<std::string>> all = db.LRange(key, 0, -1);
      for (const std::string& element : *all) out += element + ",";
      return out;
    }
    case KeyType::kSet: {
      std::string out = "set:";
      StatusOr<std::vector<std::string>> all = db.SMembers(key);
      for (const std::string& member : *all) out += member + ",";
      return out;
    }
    case KeyType::kZSet: {
      std::string out = "zset:";
      StatusOr<std::vector<ScoredMember>> all = db.ZRange(key, 0, -1);
      for (const ScoredMember& m : *all) {
        char score[40];
        std::snprintf(score, sizeof score, "%.17g", m.score);
        out += m.member + "=" + score + ",";
      }
      return out;
    }
    case KeyType::kStream: {
      std::string out =
          "stream:last=" + db.XLastId(key)->value().ToString() + ";";
      StatusOr<std::vector<StreamEntry>> all =
          db.XRange(key, StreamId(), std::numeric_limits<size_t>::max());
      for (const StreamEntry& e : *all) {
        out += e.id.ToString() + "=" + e.payload + ",";
      }
      return out;
    }
    default:
      return "?";
  }
}

// Two calls on two Dbs that should agree must fail the same way, or both
// succeed with the same result.
template <typename T>
void ExpectSame(const StatusOr<T>& a, const StatusOr<T>& b) {
  EXPECT_EQ(a.status().code(), b.status().code());
  if (a.ok() && b.ok()) {
    EXPECT_EQ(*a, *b);
  }
}

TEST_F(PersistTest, BatchSurvivesReopenAsOneRecord) {
  uint64_t before;
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("old", "v").ok());
    ASSERT_TRUE(db->Set("k", "1", seconds(100)).ok());
    before = file_.Size();

    WriteBatch batch;
    batch.Set("a", "1").Set("b", "2").Del("old").Rename("k", "k2").Incr("n");
    ASSERT_TRUE(db->Write(batch).ok());
    // One frame (8) holding: type (1) + count (4) + 5 members, each with a
    // length (4) and a body.
    EXPECT_GT(file_.Size(), before);
  }
  Restart(seconds(10));
  auto db = Open();
  EXPECT_EQ(db->recovery_info().records_replayed, 3);  // 2 sets + 1 batch
  EXPECT_EQ(KeysOf(*db), (std::vector<std::string>{"a", "b", "k2", "n"}));
  EXPECT_EQ(db->Get("n"), "1");
  EXPECT_EQ(db->GetTtl("k2").remaining, seconds(90));
}

TEST_F(PersistTest, OneEffectiveChangeIsLoggedAsAPlainRecord) {
  auto db = Open();
  const uint64_t before = file_.Size();
  WriteBatch batch;
  batch.Del("missing").Set("a", "1").Expire("missing", seconds(1));
  ASSERT_TRUE(db->Write(batch).ok());
  // A plain SET: frame (8) + type (1) + key (4 + 1) + value (4 + 1) + expiry
  // (8).
  EXPECT_EQ(file_.Size() - before, 27);
}

TEST_F(PersistTest, BatchOfNoOpsWritesNothing) {
  auto db = Open();
  const uint64_t before = file_.Size();
  WriteBatch batch;
  batch.Del("x").Expire("x", seconds(1)).Persist("x").Rename("x", "y");
  ASSERT_TRUE(db->Write(batch).ok());
  EXPECT_EQ(file_.Size(), before);
}

TEST_F(PersistTest, BatchIsAllOrNothingWhateverByteACrashStopsAt) {
  uint64_t before;
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("old", "v").ok());
    ASSERT_TRUE(db->Set("k", "1", seconds(100)).ok());
    before = file_.Size();
    WriteBatch batch;
    batch.Set("new1", "a")
        .Set("new2", "b", seconds(5))
        .Del("old")
        .Rename("k", "k2")
        .Incr("n")
        .Append("new1", "!");
    ASSERT_TRUE(db->Write(batch).ok());
  }
  const std::string full = file_.data();
  ASSERT_GT(full.size(), before + 30);

  const std::vector<std::string> before_keys = {"k", "old"};
  const std::vector<std::string> after_keys = {"k2", "n", "new1", "new2"};
  for (uint64_t cut = before; cut <= full.size(); ++cut) {
    MemFile crashed(full.substr(0, cut));
    StatusOr<std::unique_ptr<Db>> db =
        Db::Open(std::make_unique<MemStorage>(&crashed), Options());
    ASSERT_TRUE(db.ok()) << "cut=" << cut;
    const bool complete = cut == full.size();
    EXPECT_EQ(KeysOf(**db), complete ? after_keys : before_keys)
        << "cut=" << cut;
    if (complete) {
      EXPECT_EQ((*db)->Get("new1"), "a!");
      EXPECT_EQ((*db)->Get("n"), "1");
    } else {
      EXPECT_EQ((*db)->Get("old"), "v");
      EXPECT_EQ((*db)->recovery_info().dropped_bytes, cut - before)
          << "cut=" << cut;
    }
  }
}

TEST_F(PersistTest, BatchThatFailsToLogIsUndoneInMemory) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1", seconds(100)).ok());
  ASSERT_TRUE(db->Set("b", "2").ok());
  ASSERT_EQ(db->Keys().size(), 2);  // builds the key index
  const uint64_t size = file_.Size();

  WriteBatch batch;
  batch.Set("new", "x").Del("b").Rename("a", "c").Incr("n");

  file_.FailAppendsBeyond(size + 3);
  EXPECT_EQ(db->Write(batch).code(), StatusCode::kIoError);
  EXPECT_EQ(file_.Size(), size);  // the partial record was removed
  EXPECT_EQ(KeysOf(*db), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(db->GetTtl("a").remaining, seconds(100));
  EXPECT_EQ(db->Get("b"), "2");

  // Nothing was lost by trying: the same batch now goes through.
  file_.FailAppendsBeyond(std::nullopt);
  ASSERT_TRUE(db->Write(batch).ok());
  EXPECT_EQ(KeysOf(*db), (std::vector<std::string>{"c", "n", "new"}));
  EXPECT_EQ(db->GetTtl("c").remaining, seconds(100));
}

TEST_F(PersistTest, BatchThatFailsToSyncIsUndoneInMemory) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1").ok());
  file_.FailSyncs(true);
  WriteBatch batch;
  batch.Set("b", "2").Del("a");
  EXPECT_EQ(db->Write(batch).code(), StatusCode::kIoError);
  EXPECT_EQ(db->Get("a"), "1");
  EXPECT_FALSE(db->Exists("b"));
  file_.FailSyncs(false);
  EXPECT_EQ(db->Set("c", "3").code(), StatusCode::kIoError);  // Db has failed
}

TEST_F(PersistTest, FailedBatchLogsNothing) {
  auto db = Open();
  ASSERT_TRUE(db->Set("text", "abc").ok());
  const uint64_t size = file_.Size();
  WriteBatch batch;
  batch.Set("a", "1").Incr("text");
  EXPECT_EQ(db->Write(batch).code(), StatusCode::kNotInteger);
  EXPECT_EQ(file_.Size(), size);
  EXPECT_FALSE(db->Exists("a"));
}

TEST_F(PersistTest, BatchAppendToAnExpiredKeyIsLoggedAsSet) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("k", "old", seconds(10)).ok());
    Advance(seconds(10));  // expired, but still in memory
    WriteBatch batch;
    batch.Append("k", "new").Set("z", "1");
    ASSERT_TRUE(db->Write(batch).ok());
  }
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(db->Get("k"), "new");  // not "oldnew"
  EXPECT_EQ(db->GetTtl("k").state, Db::TtlState::kNoExpiry);
}

TEST_F(PersistTest, TtlsDoNotDriftWhenTheWallClockMovesBetweenReads) {
  OpenOptions o = Options();
  o.wall_now = [this] {  // ticks 1 ms every time it is read
    wall_ += milliseconds(1);
    return wall_;
  };
  auto db = Open(o);

  WriteBatch batch;
  batch.Set("a", "1", seconds(10)).Set("b", "2").Expire("b", seconds(20));
  batch.Set("n", "5", seconds(40)).Incr("n");
  ASSERT_TRUE(db->Write(batch).ok());
  ASSERT_TRUE(db->Set("c", "3", seconds(30)).ok());
  ASSERT_EQ(*db->Incr("c"), 4);  // keeps its TTL

  EXPECT_EQ(db->GetTtl("a").remaining, seconds(10));
  EXPECT_EQ(db->GetTtl("b").remaining, seconds(20));
  EXPECT_EQ(db->GetTtl("n").remaining, seconds(40));
  EXPECT_EQ(db->GetTtl("c").remaining, seconds(30));

  ASSERT_TRUE(db->Compact().ok());  // and compaction keeps them as they were
  EXPECT_EQ(db->GetTtl("a").remaining, seconds(10));
  EXPECT_EQ(db->GetTtl("n").remaining, seconds(40));
}

// Compaction in tiny pieces, so that a few hundred elements span many records.
OpenOptions SmallChunks(OpenOptions o) {
  o.compact_chunk_bytes = 2048;
  return o;
}

TEST_F(PersistTest, HashesAndTheirTtlsSurviveRestart) {
  {
    auto db = Open();
    ASSERT_TRUE(db->HSet("h", {{"a", "1"}, {"b", "2"}, {"c", "3"}}).ok());
    ASSERT_TRUE(db->HSet("h", "b", "20").ok());
    ASSERT_EQ(*db->HDel("h", {"c"}), 1);
    ASSERT_EQ(*db->HIncr("h", "n", 5), 5);
    ASSERT_TRUE(*db->Expire("h", seconds(100)));
    ASSERT_TRUE(db->HSet("gone", "f", "v").ok());
    ASSERT_EQ(*db->HDel("gone", {"f"}), 1);  // empties the hash: key removed
  }
  Restart(seconds(40));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "h"), "hash:a=1;b=20;n=5;");
  EXPECT_EQ(db->GetTtl("h").remaining, seconds(60));
  EXPECT_EQ(db->Type("gone"), KeyType::kNone);
  EXPECT_EQ(db->Keys(), std::vector<std::string>{"h"});
}

TEST_F(PersistTest, ANewHashDoesNotInheritAStaleExpiredEntryAfterRestart) {
  // "k" expires without anyone touching it, so the log still holds the old
  // value and its deadline. Creating a hash under that name afterwards has to
  // replace all of it, or recovery would merge into (or discard) the stale
  // entry and disagree with what the live Db showed.
  {
    auto db = Open();
    ASSERT_TRUE(db->HSet("h", {{"old", "1"}, {"older", "2"}}).ok());
    ASSERT_TRUE(*db->Expire("h", seconds(10)));
    ASSERT_TRUE(db->Set("s", "string", seconds(10)).ok());
    ASSERT_TRUE(db->Set("t", "string", seconds(10)).ok());
    Advance(seconds(10));  // all three expire, unnoticed

    ASSERT_EQ(*db->HSet("h", "new", "3"), 1);   // stale hash
    ASSERT_EQ(*db->HSet("s", "f", "v"), 1);     // stale string
    ASSERT_EQ(*db->HIncr("t", "count", 4), 4);  // stale string, via HIncr
    ASSERT_EQ(Dump(*db, "h"), "hash:new=3;");
  }
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "h"), "hash:new=3;");
  EXPECT_EQ(Dump(*db, "s"), "hash:f=v;");
  EXPECT_EQ(Dump(*db, "t"), "hash:count=4;");
  for (const char* k : {"h", "s", "t"}) {
    EXPECT_EQ(db->GetTtl(k).state, Db::TtlState::kNoExpiry) << k;
  }
}

TEST_F(PersistTest, CompactionKeepsHashesInPiecesThatFitARecord) {
  // Far more than one 2 KiB compaction chunk.
  std::vector<FieldValue> fields;
  for (int i = 0; i < 600; ++i) {
    fields.emplace_back("field-" + std::to_string(100000 + i),
                        std::string(90, static_cast<char>('a' + i % 26)));
  }
  {
    auto db = Open(SmallChunks(Options()));
    ASSERT_TRUE(db->HSet("big", fields).ok());
    ASSERT_TRUE(*db->Expire("big", seconds(500)));
    ASSERT_TRUE(db->HSet("small", "f", "v").ok());
    ASSERT_TRUE(db->Compact().ok());
    EXPECT_EQ(storage_->replace_count(), 1);
  }
  Restart(seconds(100));
  auto db = Open();
  EXPECT_EQ(*db->HLen("big"), 600);
  EXPECT_EQ(*db->HGetAll("big"), fields);
  EXPECT_EQ(db->GetTtl("big").remaining, seconds(400));
  EXPECT_EQ(Dump(*db, "small"), "hash:f=v;");
  // Many records, not one: the big hash was split into 2 KiB pieces.
  EXPECT_GE(db->recovery_info().records_replayed, 20);
}

TEST_F(PersistTest, HashBatchesAreAllOrNothingWhateverByteACrashStopsAt) {
  uint64_t before;
  {
    auto db = Open();
    ASSERT_TRUE(db->HSet("h", {{"a", "1"}}).ok());
    before = file_.Size();
    WriteBatch batch;
    batch.HSet("h", "b", "2")
        .HDel("h", {"a"})
        .HSet("g", "x", "y")
        .HIncr("g", "n");
    ASSERT_TRUE(db->Write(batch).ok());
  }
  const std::string full = file_.data();
  for (uint64_t cut = before; cut <= full.size(); ++cut) {
    MemFile crashed(full.substr(0, cut));
    StatusOr<std::unique_ptr<Db>> db =
        Db::Open(std::make_unique<MemStorage>(&crashed), Options());
    ASSERT_TRUE(db.ok()) << "cut=" << cut;
    if (cut < full.size()) {
      EXPECT_EQ(Dump(**db, "h"), "hash:a=1;") << "cut=" << cut;
      EXPECT_EQ(Dump(**db, "g"), "none") << "cut=" << cut;
    } else {
      EXPECT_EQ(Dump(**db, "h"), "hash:b=2;");
      EXPECT_EQ(Dump(**db, "g"), "hash:n=1;x=y;");
    }
  }
}

TEST_F(PersistTest, FailedHashWritesAreUndone) {
  auto db = Open();
  ASSERT_TRUE(db->HSet("h", {{"a", "1"}}).ok());
  const uint64_t size = file_.Size();
  file_.FailAppendsBeyond(size + 3);
  EXPECT_EQ(db->HSet("h", "b", "2").status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->HDel("h", {"a"}).status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->HIncr("h", "n").status().code(), StatusCode::kIoError);
  EXPECT_EQ(file_.Size(), size);
  EXPECT_EQ(Dump(*db, "h"), "hash:a=1;");
}

TEST_F(PersistTest, ListsAndTheirTtlsSurviveRestart) {
  {
    auto db = Open();
    ASSERT_EQ(*db->RPush("l", {"c", "d", "e"}), 3);
    ASSERT_EQ(*db->LPush("l", {"b", "a"}), 5);  // a b c d e
    ASSERT_EQ(*db->RPop("l"), std::vector<std::string>{"e"});
    ASSERT_TRUE(*db->LSet("l", -1, "D"));
    ASSERT_TRUE(*db->Expire("l", seconds(100)));
    ASSERT_EQ(*db->RPush("gone", {"x"}), 1);
    ASSERT_EQ(db->LPop("gone")->size(), 1);  // empties the list: key removed
    ASSERT_EQ(*db->RPush("trim", {"1", "2", "3", "4", "5"}), 5);
    ASSERT_TRUE(db->LTrim("trim", 1, -2).ok());
    ASSERT_EQ(*db->RPush("none", {"1", "2"}), 2);
    ASSERT_TRUE(db->LTrim("none", 5, 9).ok());  // keeps nothing
  }
  Restart(seconds(30));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "l"), "list:a,b,c,D,");
  EXPECT_EQ(db->GetTtl("l").remaining, seconds(70));
  EXPECT_EQ(Dump(*db, "trim"), "list:2,3,4,");
  EXPECT_EQ(db->Type("gone"), KeyType::kNone);
  EXPECT_EQ(db->Type("none"), KeyType::kNone);
}

TEST_F(PersistTest, ANewListDoesNotInheritAStaleExpiredEntryAfterRestart) {
  {
    auto db = Open();
    ASSERT_TRUE(db->RPush("l", {"old1", "old2"}).ok());
    ASSERT_TRUE(*db->Expire("l", seconds(10)));
    ASSERT_TRUE(db->Set("s", "string", seconds(10)).ok());
    ASSERT_TRUE(db->HSet("h", "f", "v").ok());
    ASSERT_TRUE(*db->Expire("h", seconds(10)));
    Advance(seconds(10));  // all three expire, unnoticed

    ASSERT_EQ(*db->RPush("l", {"new"}), 1);
    ASSERT_EQ(*db->LPush("s", {"a", "b"}), 2);
    ASSERT_EQ(*db->RPush("h", {"x"}), 1);
  }
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "l"), "list:new,");
  EXPECT_EQ(Dump(*db, "s"), "list:b,a,");
  EXPECT_EQ(Dump(*db, "h"), "list:x,");
  for (const char* k : {"l", "s", "h"}) {
    EXPECT_EQ(db->GetTtl(k).state, Db::TtlState::kNoExpiry) << k;
  }
}

TEST_F(PersistTest, CompactionKeepsABigListInOrderAndInPieces) {
  std::vector<std::string> elements;
  for (int i = 0; i < 700; ++i) {
    elements.push_back(std::to_string(1000000 + i) + std::string(80, 'x'));
  }
  {
    auto db = Open(SmallChunks(Options()));
    ASSERT_TRUE(db->RPush("big", elements).ok());
    ASSERT_TRUE(*db->Expire("big", seconds(500)));
    ASSERT_TRUE(db->Compact().ok());
  }
  Restart(seconds(100));
  auto db = Open();
  EXPECT_EQ(*db->LLen("big"), 700);
  EXPECT_EQ(*db->LRange("big", 0, -1), elements);
  EXPECT_EQ(db->GetTtl("big").remaining, seconds(400));
  EXPECT_GE(db->recovery_info().records_replayed, 20);  // pieces, plus the TTL
}

TEST_F(PersistTest, ListBatchesAreAllOrNothingWhateverByteACrashStopsAt) {
  uint64_t before;
  {
    auto db = Open();
    ASSERT_TRUE(db->RPush("l", {"a", "b", "c"}).ok());
    before = file_.Size();
    WriteBatch batch;
    batch.LPush("l", {"z"})
        .RPop("l", 2)
        .RPush("q", {"1", "2"})
        .LSet("q", 0, "X");
    ASSERT_TRUE(db->Write(batch).ok());
  }
  const std::string full = file_.data();
  for (uint64_t cut = before; cut <= full.size(); ++cut) {
    MemFile crashed(full.substr(0, cut));
    StatusOr<std::unique_ptr<Db>> db =
        Db::Open(std::make_unique<MemStorage>(&crashed), Options());
    ASSERT_TRUE(db.ok()) << "cut=" << cut;
    if (cut < full.size()) {
      EXPECT_EQ(Dump(**db, "l"), "list:a,b,c,") << "cut=" << cut;
      EXPECT_EQ(Dump(**db, "q"), "none") << "cut=" << cut;
    } else {
      EXPECT_EQ(Dump(**db, "l"), "list:z,a,");
      EXPECT_EQ(Dump(**db, "q"), "list:X,2,");
    }
  }
}

TEST_F(PersistTest, FailedListWritesAreUndone) {
  auto db = Open();
  ASSERT_TRUE(db->RPush("l", {"a", "b"}).ok());
  const uint64_t size = file_.Size();
  file_.FailAppendsBeyond(size + 3);
  EXPECT_EQ(db->LPush("l", {"x"}).status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->RPop("l").status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->LSet("l", 0, "x").status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->LTrim("l", 0, 0).code(), StatusCode::kIoError);
  EXPECT_EQ(file_.Size(), size);
  EXPECT_EQ(Dump(*db, "l"), "list:a,b,");
}

TEST_F(PersistTest, SetsAndTheirTtlsSurviveRestart) {
  {
    auto db = Open();
    ASSERT_EQ(*db->SAdd("s", {"c", "a", "b", "a"}), 3);
    ASSERT_EQ(*db->SRem("s", {"b", "nope"}), 1);
    ASSERT_EQ(*db->SAdd("s", {"d"}), 1);
    ASSERT_TRUE(*db->Expire("s", seconds(100)));
    ASSERT_EQ(*db->SAdd("gone", {"x"}), 1);
    ASSERT_EQ(*db->SRem("gone", {"x"}), 1);  // empties the set: key removed
  }
  Restart(seconds(30));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "s"), "set:a,c,d,");
  EXPECT_EQ(db->GetTtl("s").remaining, seconds(70));
  EXPECT_EQ(db->Type("gone"), KeyType::kNone);
}

TEST_F(PersistTest, ANewSetDoesNotInheritAStaleExpiredEntryAfterRestart) {
  {
    auto db = Open();
    ASSERT_TRUE(db->SAdd("a", {"old1", "old2"}).ok());
    ASSERT_TRUE(*db->Expire("a", seconds(10)));
    ASSERT_TRUE(db->RPush("b", {"x"}).ok());
    ASSERT_TRUE(*db->Expire("b", seconds(10)));
    Advance(seconds(10));  // both expire, unnoticed

    ASSERT_EQ(*db->SAdd("a", {"new"}), 1);
    ASSERT_EQ(*db->SAdd("b", {"y"}), 1);  // was a list
  }
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "a"), "set:new,");
  EXPECT_EQ(Dump(*db, "b"), "set:y,");
  EXPECT_EQ(db->GetTtl("a").state, Db::TtlState::kNoExpiry);
  EXPECT_EQ(db->GetTtl("b").state, Db::TtlState::kNoExpiry);
}

TEST_F(PersistTest, CompactionKeepsABigSetInPieces) {
  std::vector<std::string> members;
  for (int i = 0; i < 700; ++i) {
    members.push_back(std::to_string(1000000 + i) + std::string(80, 'm'));
  }
  {
    auto db = Open(SmallChunks(Options()));
    ASSERT_TRUE(db->SAdd("big", members).ok());
    ASSERT_TRUE(*db->Expire("big", seconds(500)));
    ASSERT_TRUE(db->Compact().ok());
  }
  Restart(seconds(100));
  auto db = Open();
  EXPECT_EQ(*db->SCard("big"), 700);
  EXPECT_EQ(*db->SMembers("big"), members);  // already in byte order
  EXPECT_EQ(db->GetTtl("big").remaining, seconds(400));
  EXPECT_GE(db->recovery_info().records_replayed, 20);
}

TEST_F(PersistTest, SetBatchesAreAllOrNothingWhateverByteACrashStopsAt) {
  uint64_t before;
  {
    auto db = Open();
    ASSERT_TRUE(db->SAdd("s", {"a", "b"}).ok());
    before = file_.Size();
    WriteBatch batch;
    batch.SAdd("s", {"c"}).SRem("s", {"a"}).SAdd("t", {"x", "y"});
    ASSERT_TRUE(db->Write(batch).ok());
  }
  const std::string full = file_.data();
  for (uint64_t cut = before; cut <= full.size(); ++cut) {
    MemFile crashed(full.substr(0, cut));
    StatusOr<std::unique_ptr<Db>> db =
        Db::Open(std::make_unique<MemStorage>(&crashed), Options());
    ASSERT_TRUE(db.ok()) << "cut=" << cut;
    if (cut < full.size()) {
      EXPECT_EQ(Dump(**db, "s"), "set:a,b,") << "cut=" << cut;
      EXPECT_EQ(Dump(**db, "t"), "none") << "cut=" << cut;
    } else {
      EXPECT_EQ(Dump(**db, "s"), "set:b,c,");
      EXPECT_EQ(Dump(**db, "t"), "set:x,y,");
    }
  }
}

TEST_F(PersistTest, FailedSetWritesAreUndone) {
  auto db = Open();
  ASSERT_TRUE(db->SAdd("s", {"a", "b"}).ok());
  const uint64_t size = file_.Size();
  file_.FailAppendsBeyond(size + 3);
  EXPECT_EQ(db->SAdd("s", {"c"}).status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->SRem("s", {"a"}).status().code(), StatusCode::kIoError);
  EXPECT_EQ(file_.Size(), size);
  EXPECT_EQ(Dump(*db, "s"), "set:a,b,");
}

TEST_F(PersistTest, SortedSetsKeepExactScoresAndTtlsAcrossRestart) {
  {
    auto db = Open();
    ASSERT_EQ(
        *db->ZAdd("z",
                  {{"a", 0.1}, {"b", 1.0 / 3.0}, {"c", -2}, {"d", 5e-324}}),
        4);
    ASSERT_EQ(*db->ZAdd("z", {{"a", 0.2}}), 0);
    ASSERT_EQ(*db->ZRem("z", {"c"}), 1);
    ASSERT_EQ(*db->ZIncr("z", "b", 1), 1.0 / 3.0 + 1);
    ASSERT_TRUE(*db->Expire("z", seconds(100)));
    ASSERT_EQ(*db->ZAdd("gone", {{"x", 1}}), 1);
    ASSERT_EQ(*db->ZRem("gone", {"x"}), 1);  // empties it: key removed
  }
  Restart(seconds(30));
  auto db = Open();
  EXPECT_EQ(**db->ZScore("z", "a"), 0.2);
  EXPECT_EQ(**db->ZScore("z", "b"), 1.0 / 3.0 + 1);
  EXPECT_EQ(**db->ZScore("z", "d"), 5e-324);
  EXPECT_EQ(*db->ZCard("z"), 3);
  EXPECT_EQ(**db->ZRank("z", "d"), 0);  // the order index was rebuilt
  EXPECT_EQ(db->GetTtl("z").remaining, seconds(70));
  EXPECT_EQ(db->Type("gone"), KeyType::kNone);
}

TEST_F(PersistTest, ANewSortedSetDoesNotInheritAStaleExpiredEntryAfterRestart) {
  {
    auto db = Open();
    ASSERT_TRUE(db->ZAdd("a", {{"old", 1}}).ok());
    ASSERT_TRUE(*db->Expire("a", seconds(10)));
    ASSERT_TRUE(db->SAdd("b", {"x"}).ok());
    ASSERT_TRUE(*db->Expire("b", seconds(10)));
    Advance(seconds(10));  // both expire, unnoticed

    ASSERT_EQ(*db->ZAdd("a", {{"new", 2}}), 1);
    ASSERT_EQ(*db->ZIncr("b", "y", 3), 3);  // was a set
  }
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "a"), "zset:new=2,");
  EXPECT_EQ(Dump(*db, "b"), "zset:y=3,");
  EXPECT_EQ(db->GetTtl("a").state, Db::TtlState::kNoExpiry);
  EXPECT_EQ(db->GetTtl("b").state, Db::TtlState::kNoExpiry);
}

TEST_F(PersistTest, CompactionKeepsABigSortedSetInPiecesWithItsScores) {
  std::vector<ScoredMember> members;
  for (int i = 0; i < 600; ++i) {
    members.push_back(
        {"member-" + std::to_string(100000 + i) + std::string(70, 'z'),
         i * 0.25 - 1000});
  }
  {
    auto db = Open(SmallChunks(Options()));
    ASSERT_TRUE(db->ZAdd("big", members).ok());
    ASSERT_TRUE(*db->Expire("big", seconds(500)));
    ASSERT_TRUE(db->Compact().ok());
  }
  Restart(seconds(100));
  auto db = Open();
  EXPECT_EQ(*db->ZCard("big"), 600);
  EXPECT_EQ(*db->ZRange("big", 0, -1), members);  // scores rise with i
  EXPECT_EQ(db->GetTtl("big").remaining, seconds(400));
  EXPECT_GE(db->recovery_info().records_replayed, 20);
}

TEST_F(PersistTest, SortedSetBatchesAreAllOrNothingWhateverByteACrashStopsAt) {
  uint64_t before;
  {
    auto db = Open();
    ASSERT_TRUE(db->ZAdd("z", {{"a", 1}, {"b", 2}}).ok());
    before = file_.Size();
    WriteBatch batch;
    batch.ZAdd("z", {{"c", 3}}).ZRem("z", {"a"}).ZIncr("y", "m", 1.5);
    ASSERT_TRUE(db->Write(batch).ok());
  }
  const std::string full = file_.data();
  for (uint64_t cut = before; cut <= full.size(); ++cut) {
    MemFile crashed(full.substr(0, cut));
    StatusOr<std::unique_ptr<Db>> db =
        Db::Open(std::make_unique<MemStorage>(&crashed), Options());
    ASSERT_TRUE(db.ok()) << "cut=" << cut;
    if (cut < full.size()) {
      EXPECT_EQ(Dump(**db, "z"), "zset:a=1,b=2,") << "cut=" << cut;
      EXPECT_EQ(Dump(**db, "y"), "none") << "cut=" << cut;
    } else {
      EXPECT_EQ(Dump(**db, "z"), "zset:b=2,c=3,");
      EXPECT_EQ(Dump(**db, "y"), "zset:m=1.5,");
    }
  }
}

TEST_F(PersistTest, FailedSortedSetWritesAreUndone) {
  auto db = Open();
  ASSERT_TRUE(db->ZAdd("z", {{"a", 1}, {"b", 2}}).ok());
  const uint64_t size = file_.Size();
  file_.FailAppendsBeyond(size + 3);
  EXPECT_EQ(db->ZAdd("z", {{"c", 3}}).status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->ZRem("z", {"a"}).status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->ZIncr("z", "a", 5).status().code(), StatusCode::kIoError);
  EXPECT_EQ(file_.Size(), size);
  EXPECT_EQ(Dump(*db, "z"), "zset:a=1,b=2,");
}

TEST_F(PersistTest, MaxTtlSurvivesReopen) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("k", "v", Db::kMaxTtl).ok());
    EXPECT_EQ(db->Set("bad", "v", milliseconds::max()).code(),
              StatusCode::kInvalidArgument);
  }
  const uint64_t size = file_.Size();
  Restart(seconds(60));
  auto db = Open();
  EXPECT_EQ(file_.Size(), size);  // the rejected write logged nothing
  EXPECT_EQ(db->GetTtl("k").remaining, Db::kMaxTtl - seconds(60));
  EXPECT_FALSE(db->Exists("bad"));
}

TEST_F(PersistTest, AbsurdExpiryInLogIsSaturatedNotOverflowed) {
  ASSERT_TRUE(WriteHeader(file_).ok());
  const int64_t kInt64Max = std::numeric_limits<int64_t>::max();
  for (int64_t at : {kInt64Max, kInt64Max / 2, int64_t{1} << 62}) {
    ASSERT_TRUE(AppendRecord(file_, Record{Record::Type::kSet,
                                           "k" + std::to_string(at), "v", at})
                    .ok());
  }
  ASSERT_TRUE(
      AppendRecord(file_, Record{Record::Type::kSet, "past", "v", int64_t{0}})
          .ok());

  auto db = Open();
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(db->Size(), 3);  // "past" expired long ago
  for (const std::string& k :
       {"k" + std::to_string(kInt64Max), "k" + std::to_string(kInt64Max / 2),
        "k" + std::to_string(int64_t{1} << 62)}) {
    Db::TtlInfo info = db->GetTtl(k);
    EXPECT_EQ(info.state, Db::TtlState::kExpiring) << k;
    EXPECT_EQ(info.remaining, Db::kMaxTtl) << k;
  }
}

TEST_F(PersistTest, ExpirePersistAndOverwriteSurviveReopen) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("expired", "v").ok());
    ASSERT_TRUE(*db->Expire("expired", seconds(5)));
    ASSERT_TRUE(db->Set("kept", "v", seconds(5)).ok());
    ASSERT_TRUE(*db->Persist("kept"));
    ASSERT_TRUE(db->Set("cleared", "v", seconds(5)).ok());
    ASSERT_TRUE(db->Set("cleared", "w").ok());
    ASSERT_TRUE(db->Set("now", "v").ok());
    ASSERT_TRUE(*db->Expire("now", milliseconds(0)));
  }
  Restart(seconds(60));
  auto db = Open();
  EXPECT_FALSE(db->Exists("expired"));
  EXPECT_EQ(db->GetTtl("kept").state, Db::TtlState::kNoExpiry);
  EXPECT_EQ(db->GetTtl("cleared").state, Db::TtlState::kNoExpiry);
  EXPECT_EQ(db->Get("cleared"), "w");
  EXPECT_FALSE(db->Exists("now"));
}

TEST_F(PersistTest, IncrNeverShortensTheTtlEvenAcrossManyCallsAndReopens) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("n", "0", seconds(100)).ok());
    for (int i = 0; i < 50; ++i) {
      steady_ += std::chrono::microseconds(500);
      wall_ += std::chrono::microseconds(500);
      ASSERT_TRUE(db->Incr("n").ok());
    }
    EXPECT_EQ(db->GetTtl("n").remaining, seconds(100) - milliseconds(25));
  }
  // The log carries the absolute deadline from the Set, untouched by the Incrs.
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(db->Get("n"), "50");
  EXPECT_EQ(db->GetTtl("n").remaining, seconds(100) - milliseconds(25));
}

TEST_F(PersistTest, IncrKeepsTtlAcrossReopen) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("n", "10", seconds(10)).ok());
    ASSERT_EQ(*db->Incr("n"), 11);
  }
  Restart(seconds(3));
  auto db = Open();
  EXPECT_EQ(db->Get("n"), "11");
  EXPECT_EQ(db->GetTtl("n").remaining, seconds(7));
}

TEST_F(PersistTest, OperationsThatChangeNothingWriteNothing) {
  auto db = Open();
  ASSERT_TRUE(db->Set("plain", "abc").ok());
  ASSERT_TRUE(db->Set("short", "v", seconds(1)).ok());
  Advance(seconds(2));
  const uint64_t size = file_.Size();

  EXPECT_FALSE(*db->Del("missing"));
  EXPECT_FALSE(*db->Del("short"));  // already expired
  EXPECT_FALSE(*db->Expire("missing", seconds(1)));
  EXPECT_FALSE(*db->Persist("plain"));  // no TTL
  EXPECT_FALSE(*db->Persist("missing"));
  EXPECT_FALSE(db->Incr("plain").ok());  // not an integer
  EXPECT_EQ(file_.Size(), size);
}

TEST_F(PersistTest, SyncPolicyAlwaysSyncsEveryWrite) {
  auto db = Open();
  const int base = file_.sync_count();
  ASSERT_TRUE(db->Set("a", "1").ok());
  ASSERT_TRUE(db->Set("b", "2").ok());
  EXPECT_EQ(file_.sync_count(), base + 2);
  EXPECT_EQ(db->Get("a"), "1");  // reads never sync
  EXPECT_EQ(file_.sync_count(), base + 2);
}

TEST_F(PersistTest, SyncPolicyManualSyncsOnlyWhenAsked) {
  auto db = Open(Options(SyncPolicy::kManual));
  const int base = file_.sync_count();
  ASSERT_TRUE(db->Set("a", "1").ok());
  ASSERT_TRUE(db->Set("b", "2").ok());
  EXPECT_EQ(file_.sync_count(), base);
  ASSERT_TRUE(db->Sync().ok());
  EXPECT_EQ(file_.sync_count(), base + 1);
}

// Options for SyncPolicy::kInterval driven by the fake clock, with no thread.
OpenOptions IntervalOptions(OpenOptions o) {
  o.sync = SyncPolicy::kInterval;
  o.sync_interval = seconds(1);
  o.sync_thread = false;
  return o;
}

TEST_F(PersistTest, IntervalPolicySyncsInlineOncePerInterval) {
  auto db = Open(IntervalOptions(Options()));
  const int base = file_.sync_count();
  ASSERT_TRUE(db->Set("a", "1").ok());
  EXPECT_EQ(file_.sync_count(), base);  // not due yet
  Advance(milliseconds(500));
  ASSERT_TRUE(db->Set("b", "2").ok());
  EXPECT_EQ(file_.sync_count(), base);
  Advance(milliseconds(600));
  ASSERT_TRUE(db->Set("c", "3").ok());
  EXPECT_EQ(file_.sync_count(), base + 1);  // the interval has passed
  ASSERT_TRUE(db->Set("d", "4").ok());
  EXPECT_EQ(file_.sync_count(), base + 1);  // and the timer restarted
  Advance(seconds(1));
  ASSERT_TRUE(db->Set("e", "5").ok());
  EXPECT_EQ(file_.sync_count(), base + 2);
}

TEST_F(PersistTest, SyncIfDueSyncsOnlyDirtyDataPastTheInterval) {
  auto db = Open(IntervalOptions(Options()));
  const int base = file_.sync_count();
  ASSERT_TRUE(db->SyncIfDue().ok());
  Advance(seconds(5));
  ASSERT_TRUE(db->SyncIfDue().ok());
  EXPECT_EQ(file_.sync_count(), base);  // nothing to sync

  ASSERT_TRUE(db->Set("a", "1").ok());  // due, so this write syncs itself
  EXPECT_EQ(file_.sync_count(), base + 1);
  ASSERT_TRUE(db->Set("b", "2").ok());
  ASSERT_TRUE(db->SyncIfDue().ok());
  EXPECT_EQ(file_.sync_count(), base + 1);  // dirty, but not due
  Advance(seconds(1));
  ASSERT_TRUE(db->SyncIfDue().ok());
  EXPECT_EQ(file_.sync_count(), base + 2);
  ASSERT_TRUE(db->SyncIfDue().ok());
  EXPECT_EQ(file_.sync_count(), base + 2);  // clean again
}

TEST_F(PersistTest, ExplicitSyncResetsTheIntervalTimer) {
  auto db = Open(IntervalOptions(Options()));
  ASSERT_TRUE(db->Set("a", "1").ok());
  ASSERT_TRUE(db->Sync().ok());
  const int base = file_.sync_count();
  Advance(seconds(5));
  ASSERT_TRUE(db->SyncIfDue().ok());
  EXPECT_EQ(file_.sync_count(), base);  // already clean
}

TEST_F(PersistTest, SyncIfDueDoesNothingUnderOtherPolicies) {
  for (SyncPolicy policy : {SyncPolicy::kAlways, SyncPolicy::kManual}) {
    file_.data().clear();
    auto db = Open(Options(policy));
    ASSERT_TRUE(db->Set("a", "1").ok());
    const int base = file_.sync_count();
    Advance(seconds(60));
    ASSERT_TRUE(db->SyncIfDue().ok());
    EXPECT_EQ(file_.sync_count(), base);
  }
}

TEST_F(PersistTest, DestructorSyncsWritesThatAreNotOnDisk) {
  for (SyncPolicy policy : {SyncPolicy::kManual, SyncPolicy::kInterval}) {
    file_.data().clear();
    OpenOptions o = IntervalOptions(Options(policy));
    o.sync = policy;
    auto db = Open(o);
    ASSERT_TRUE(db->Set("a", "1").ok());
    const int base = file_.sync_count();
    db.reset();
    EXPECT_EQ(file_.sync_count(), base + 1);
  }

  file_.data().clear();
  auto db = Open();  // kAlways has nothing left to sync
  ASSERT_TRUE(db->Set("a", "1").ok());
  const int base = file_.sync_count();
  db.reset();
  EXPECT_EQ(file_.sync_count(), base);
}

TEST_F(PersistTest, IntervalSyncFailureFailsTheWrite) {
  auto db = Open(IntervalOptions(Options()));
  ASSERT_TRUE(db->Set("a", "1").ok());
  Advance(seconds(1));  // the next write will sync

  file_.FailSyncs(true);
  Status s = db->Set("b", "2");
  EXPECT_EQ(s.code(), StatusCode::kIoError);
  EXPECT_EQ(db->Get("b"), std::nullopt);

  file_.FailSyncs(false);
  Status later = db->Set("c", "3");
  EXPECT_EQ(later.code(), StatusCode::kIoError);
  EXPECT_NE(later.message().find("injected sync failure"), std::string::npos)
      << later.message();  // the first cause is reported
}

TEST_F(PersistTest, IntervalPolicyRequiresPositiveInterval) {
  for (milliseconds bad : {milliseconds(0), milliseconds(-5)}) {
    OpenOptions o = IntervalOptions(Options());
    o.sync_interval = bad;
    EXPECT_EQ(TryOpen(o).status().code(), StatusCode::kInvalidArgument);
  }
}

// The tests below use the real clock for the background thread, with a short
// interval, and wait for the effect instead of sleeping a fixed time.
bool WaitFor(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + seconds(10);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(milliseconds(2));
  }
  return true;
}

OpenOptions ThreadOptions(OpenOptions o) {
  o.sync = SyncPolicy::kInterval;
  o.sync_interval = milliseconds(10);
  o.sync_thread = true;
  return o;
}

TEST_F(PersistTest, BackgroundThreadSyncsIdleWrites) {
  auto db = Open(ThreadOptions(Options()));
  const int base = file_.sync_count();
  ASSERT_TRUE(db->Set("a", "1").ok());  // no further writes follow
  EXPECT_TRUE(WaitFor([&] { return file_.sync_count() > base; }));
}

TEST_F(PersistTest, BackgroundThreadDoesNotSyncWhenThereIsNothingToSync) {
  auto db = Open(ThreadOptions(Options()));
  const int base = file_.sync_count();
  std::this_thread::sleep_for(milliseconds(150));  // many intervals
  EXPECT_EQ(file_.sync_count(), base);
}

TEST_F(PersistTest, BackgroundSyncFailureSurfacesOnNextWrite) {
  auto db = Open(ThreadOptions(Options()));
  ASSERT_TRUE(db->Set("a", "1").ok());
  file_.FailSyncs(true);
  Status last;
  ASSERT_TRUE(WaitFor([&] {
    last = db->Set("x", "y");
    return !last.ok();
  }));
  EXPECT_EQ(last.code(), StatusCode::kIoError);
  EXPECT_NE(last.message().find("injected sync failure"), std::string::npos)
      << last.message();
  EXPECT_EQ(db->Sync().code(), StatusCode::kIoError);
}

TEST_F(PersistTest, DestroyingWhileThreadRunsSyncsExactlyOnce) {
  for (int i = 0; i < 20; ++i) {
    file_.data().clear();
    OpenOptions o = ThreadOptions(Options());
    o.sync_interval = milliseconds(1);  // make the thread race the destructor
    auto db = Open(o);
    const int base = file_.sync_count();
    ASSERT_TRUE(db->Set("a", "1").ok());
    db.reset();  // the thread or the destructor syncs, never both
    EXPECT_EQ(file_.sync_count(), base + 1) << "i=" << i;
  }
}

TEST_F(PersistTest, InMemoryDbSyncIsNoOp) {
  Db db;
  EXPECT_TRUE(db.Sync().ok());
  EXPECT_EQ(db.recovery_info().records_replayed, 0);
}

TEST_F(PersistTest, FailedAppendLeavesStoreAndFileUnchanged) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1").ok());
  const uint64_t size = file_.Size();

  file_.FailAppendsBeyond(size + 3);
  EXPECT_EQ(db->Set("b", "2").code(), StatusCode::kIoError);
  EXPECT_EQ(db->Get("b"), std::nullopt);
  EXPECT_EQ(file_.Size(), size);  // the partial record was removed

  file_.FailAppendsBeyond(std::nullopt);
  ASSERT_TRUE(db->Set("b", "2").ok());
  db.reset();
  db = Open();
  EXPECT_EQ(db->Get("a"), "1");
  EXPECT_EQ(db->Get("b"), "2");
  EXPECT_EQ(db->recovery_info().dropped_bytes, 0);
}

TEST_F(PersistTest, FailedAppendDoesNotApplyIncrDelOrExpire) {
  auto db = Open();
  ASSERT_TRUE(db->Set("n", "1", seconds(10)).ok());
  file_.FailAppendsBeyond(file_.Size());  // every further append fails

  EXPECT_EQ(db->Incr("n").status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->Del("n").status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->Expire("n", seconds(99)).status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->Persist("n").status().code(), StatusCode::kIoError);
  EXPECT_EQ(db->Get("n"), "1");
  EXPECT_EQ(db->GetTtl("n").remaining, seconds(10));
}

TEST_F(PersistTest, UnrepairableAppendFailureStopsWrites) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1").ok());
  const uint64_t size = file_.Size();

  file_.FailAppendsBeyond(size + 3);
  file_.FailTruncates(true);
  EXPECT_EQ(db->Set("b", "2").code(), StatusCode::kIoError);
  EXPECT_EQ(file_.Size(), size + 3);  // garbage left behind

  // Even with the fault gone, the Db refuses to append after garbage.
  file_.FailAppendsBeyond(std::nullopt);
  file_.FailTruncates(false);
  EXPECT_EQ(db->Set("c", "3").code(), StatusCode::kIoError);
  EXPECT_EQ(db->Sync().code(), StatusCode::kIoError);
  EXPECT_EQ(db->Get("a"), "1");  // reads still work
  db.reset();

  // Reopening repairs the torn tail.
  db = Open();
  EXPECT_EQ(db->recovery_info().dropped_bytes, 3);
  EXPECT_EQ(file_.Size(), size);
  EXPECT_EQ(db->Get("a"), "1");
  EXPECT_TRUE(db->Set("c", "3").ok());
}

TEST_F(PersistTest, SyncFailureStopsWritesAndLeavesMemoryUnchanged) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1").ok());

  file_.FailSyncs(true);
  EXPECT_EQ(db->Set("b", "2").code(), StatusCode::kIoError);
  EXPECT_EQ(db->Get("b"), std::nullopt);

  file_.FailSyncs(false);
  EXPECT_EQ(db->Set("c", "3").code(), StatusCode::kIoError);
  EXPECT_EQ(db->Get("a"), "1");
}

TEST_F(PersistTest, OpenDropsTornTailAndRepairsFile) {
  uint64_t good_size;
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("a", "1").ok());
    ASSERT_TRUE(db->Set("b", "2").ok());
    good_size = file_.Size();
  }
  file_.data().append("\x10\0\0\0\xaa", 5);  // half-written record

  auto db = Open();
  EXPECT_EQ(db->recovery_info().records_replayed, 2);
  EXPECT_EQ(db->recovery_info().dropped_bytes, 5);
  EXPECT_EQ(file_.Size(), good_size);
  EXPECT_EQ(db->Get("b"), "2");
  ASSERT_TRUE(db->Set("c", "3").ok());
  db.reset();

  db = Open();  // the repaired log replays cleanly
  EXPECT_EQ(db->recovery_info().dropped_bytes, 0);
  EXPECT_EQ(db->Get("c"), "3");
}

TEST_F(PersistTest, StrictRecoveryRefusesTornTailAndLeavesFileAlone) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("a", "1").ok());
  }
  file_.data().append("junk", 4);
  const std::string before = file_.data();

  StatusOr<std::unique_ptr<Db>> r =
      TryOpen(Options(SyncPolicy::kAlways, Recovery::kStrict));
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), StatusCode::kCorruption);
  EXPECT_EQ(file_.data(), before);
}

TEST_F(PersistTest, TornHeaderIsRewritten) {
  file_.data() = std::string("QEYDB\0", 6);  // header write cut short
  auto db = Open();
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(file_.Size(), kLogHeaderSize);
  ASSERT_TRUE(db->Set("a", "1").ok());
  db.reset();
  EXPECT_EQ(Open()->Get("a"), "1");
}

TEST_F(PersistTest, MidFileCorruptionFailsOpenWithoutModifyingFile) {
  {
    auto db = Open();
    ASSERT_TRUE(db->Set("a", "1").ok());
    ASSERT_TRUE(db->Set("b", "2").ok());
    ASSERT_TRUE(db->Set("c", "3").ok());
  }
  file_.data()[kLogHeaderSize + 10] ^= 0x01;  // inside the first record
  const std::string before = file_.data();

  StatusOr<std::unique_ptr<Db>> r = TryOpen(Options());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), StatusCode::kCorruption);
  EXPECT_EQ(file_.data(), before);
}

TEST_F(PersistTest, OpenRejectsForeignFileAndNullFile) {
  file_.data() = "this is not a qey database file";
  EXPECT_EQ(TryOpen(Options()).status().code(), StatusCode::kCorruption);
  EXPECT_EQ(Db::Open(std::unique_ptr<Storage>()).status().code(),
            StatusCode::kInvalidArgument);
}

// Runs random operations against a persistent Db and an in-memory Db in
// lockstep, then checks that reopening the log reproduces the same state.
TEST_F(PersistTest, ReopenedStoreMatchesLiveStore) {
  for (uint32_t seed = 1; seed <= 10; ++seed) {
    // Odd seeds compact constantly, so replay covers compacted logs.
    OpenOptions options = Options();
    if (seed % 2 == 1) {
      options.compact_min_bytes = 64;
      options.compact_ratio = 2;
    } else {
      options.auto_compact = false;
    }
    file_.data().clear();
    steady_ = Db::Clock::time_point(seconds(1000));
    wall_ = std::chrono::system_clock::time_point(seconds(1700000000));

    std::mt19937 rng(seed);
    auto pick = [&](int n) { return static_cast<int>(rng() % n); };
    auto key = [&] { return "k" + std::to_string(pick(8)); };

    auto persistent = Open(options);
    Db reference([this] { return steady_; }, [this] { return wall_; });
    ASSERT_NE(persistent, nullptr);

    for (int i = 0; i < 2000; ++i) {
      const std::string k = key();
      switch (pick(18)) {
        case 0:
          ASSERT_TRUE(persistent->Set(k, std::to_string(pick(100))).ok());
          ASSERT_TRUE(reference.Set(k, persistent->Get(k).value()).ok());
          break;
        case 1: {
          const milliseconds ttl(pick(60) - 5);
          const std::string v = std::to_string(pick(100));
          ASSERT_TRUE(persistent->Set(k, v, ttl).ok());
          ASSERT_TRUE(reference.Set(k, v, ttl).ok());
          break;
        }
        case 2:
          EXPECT_EQ(*persistent->Del(k), *reference.Del(k));
          break;
        case 3: {
          const milliseconds ttl(pick(60) - 5);
          EXPECT_EQ(*persistent->Expire(k, ttl), *reference.Expire(k, ttl));
          break;
        }
        case 4:
          EXPECT_EQ(*persistent->Persist(k), *reference.Persist(k));
          break;
        case 5: {
          const int64_t delta = pick(21) - 10;
          StatusOr<int64_t> a = persistent->Incr(k, delta);
          StatusOr<int64_t> b = reference.Incr(k, delta);
          ASSERT_EQ(a.ok(), b.ok());
          if (a.ok()) {
            EXPECT_EQ(*a, *b);
          }
          break;
        }
        case 6:
          Advance(milliseconds(pick(20)));
          break;
        case 7:
          if (pick(10) == 0) {
            ASSERT_TRUE(persistent->Compact().ok());
          }
          break;
        case 8: {
          const std::string suffix = std::to_string(pick(100));
          ExpectSame(persistent->Append(k, suffix),
                     reference.Append(k, suffix));
          break;
        }
        case 9: {
          const std::string to = key();
          EXPECT_EQ(*persistent->Rename(k, to), *reference.Rename(k, to));
          break;
        }
        case 10:
          if (pick(40) == 0) {
            ASSERT_TRUE(persistent->Clear().ok());
            ASSERT_TRUE(reference.Clear().ok());
          }
          break;
        case 17: {
          const std::string payload = "p" + std::to_string(pick(100));
          switch (pick(7)) {
            case 0:
              ExpectSame(persistent->XAdd(k, payload),
                         reference.XAdd(k, payload));
              break;
            case 1: {
              const size_t n = pick(4);
              ExpectSame(persistent->XAdd(k, payload, n),
                         reference.XAdd(k, payload, n));
              break;
            }
            case 2: {
              const size_t n = pick(4);
              ExpectSame(persistent->XTrimMaxLen(k, n),
                         reference.XTrimMaxLen(k, n));
              break;
            }
            case 3: {
              const StreamId id{1700000000000u + pick(40), 0};
              ExpectSame(persistent->XTrimMinId(k, id),
                         reference.XTrimMinId(k, id));
              break;
            }
            case 4:
              ExpectSame(persistent->XRange(k, StreamId(), 3),
                         reference.XRange(k, StreamId(), 3));
              break;
            case 5:
              ExpectSame(persistent->XRevRange(k, 3),
                         reference.XRevRange(k, 3));
              break;
            case 6:
              ExpectSame(persistent->XLastId(k), reference.XLastId(k));
              break;
          }
          break;
        }
        case 16: {
          const std::string m = "m" + std::to_string(pick(6));
          const double score = pick(7) - 3;
          switch (pick(7)) {
            case 0:
              ExpectSame(persistent->ZAdd(k, {{m, score}, {"m9", score * 2}}),
                         reference.ZAdd(k, {{m, score}, {"m9", score * 2}}));
              break;
            case 1:
              ExpectSame(persistent->ZRem(k, {m}), reference.ZRem(k, {m}));
              break;
            case 2:
              ExpectSame(persistent->ZIncr(k, m, score / 4),
                         reference.ZIncr(k, m, score / 4));
              break;
            case 3:
              ExpectSame(persistent->ZRank(k, m), reference.ZRank(k, m));
              break;
            case 4:
              ExpectSame(persistent->ZScore(k, m), reference.ZScore(k, m));
              break;
            case 5:
              ExpectSame(persistent->ZRange(k, -2, 2),
                         reference.ZRange(k, -2, 2));
              break;
            case 6:
              ExpectSame(persistent->ZRangeByScore(k, -1, 1),
                         reference.ZRangeByScore(k, -1, 1));
              break;
          }
          break;
        }
        case 15: {
          const std::string m = std::to_string(pick(6));
          switch (pick(6)) {
            case 0:
              ExpectSame(persistent->SAdd(k, {m, m + "b"}),
                         reference.SAdd(k, {m, m + "b"}));
              break;
            case 1:
              ExpectSame(persistent->SRem(k, {m, "1"}),
                         reference.SRem(k, {m, "1"}));
              break;
            case 2:
              ExpectSame(persistent->SIsMember(k, m),
                         reference.SIsMember(k, m));
              break;
            case 3:
              ExpectSame(persistent->SMembers(k), reference.SMembers(k));
              break;
            case 4: {
              const std::string k2 = key();
              ExpectSame(persistent->SUnion({k, k2}),
                         reference.SUnion({k, k2}));
              ExpectSame(persistent->SInter({k, k2}),
                         reference.SInter({k, k2}));
              ExpectSame(persistent->SDiff({k, k2}), reference.SDiff({k, k2}));
              break;
            }
            case 5:
              ExpectSame(persistent->SCard(k), reference.SCard(k));
              break;
          }
          break;
        }
        case 14: {
          const std::string v = std::to_string(pick(50));
          switch (pick(8)) {
            case 0:
              ExpectSame(persistent->LPush(k, {v, v + "x"}),
                         reference.LPush(k, {v, v + "x"}));
              break;
            case 1:
              ExpectSame(persistent->RPush(k, {v}), reference.RPush(k, {v}));
              break;
            case 2: {
              const size_t n = pick(3);
              ExpectSame(persistent->LPop(k, n), reference.LPop(k, n));
              break;
            }
            case 3: {
              const size_t n = pick(3);
              ExpectSame(persistent->RPop(k, n), reference.RPop(k, n));
              break;
            }
            case 4: {
              const int64_t i = pick(7) - 3;
              ExpectSame(persistent->LSet(k, i, v), reference.LSet(k, i, v));
              break;
            }
            case 5: {
              const int64_t a = pick(9) - 4;
              const int64_t b = pick(9) - 4;
              EXPECT_EQ(persistent->LTrim(k, a, b).code(),
                        reference.LTrim(k, a, b).code());
              break;
            }
            case 6: {
              const int64_t i = pick(7) - 3;
              ExpectSame(persistent->LIndex(k, i), reference.LIndex(k, i));
              break;
            }
            case 7:
              ExpectSame(persistent->LRange(k, -3, 3),
                         reference.LRange(k, -3, 3));
              break;
          }
          break;
        }
        case 13: {
          const std::string field = "f" + std::to_string(pick(4));
          switch (pick(3)) {
            case 0: {
              const std::string v = std::to_string(pick(50));
              ExpectSame(persistent->HSet(k, field, v),
                         reference.HSet(k, field, v));
              break;
            }
            case 1:
              ExpectSame(persistent->HDel(k, {field}),
                         reference.HDel(k, {field}));
              break;
            case 2: {
              const int64_t delta = pick(21) - 10;
              ExpectSame(persistent->HIncr(k, field, delta),
                         reference.HIncr(k, field, delta));
              break;
            }
          }
          break;
        }
        case 12: {
          WriteBatch batch;
          const int n = 1 + pick(4);
          for (int j = 0; j < n; ++j) {
            const std::string bk = key();
            switch (pick(19)) {
              case 16:
                batch.ZAdd(bk, {{"m" + std::to_string(pick(6)),
                                 static_cast<double>(pick(7) - 3)}});
                break;
              case 17:
                batch.ZRem(bk, {"m" + std::to_string(pick(6))});
                break;
              case 18:
                batch.ZIncr(bk, "m" + std::to_string(pick(6)), pick(5) - 2);
                break;
              case 14:
                batch.SAdd(bk, {std::to_string(pick(6))});
                break;
              case 15:
                batch.SRem(bk, {std::to_string(pick(6))});
                break;
              case 11:
                batch.RPush(bk, {std::to_string(pick(50))});
                break;
              case 12:
                if (pick(2) == 0) {
                  batch.LPop(bk, pick(3));
                } else {
                  batch.RPop(bk, pick(3));
                }
                break;
              case 13:
                batch.LTrim(bk, pick(5) - 2, pick(5) - 2);
                break;
              case 8:
                batch.HSet(bk, "f" + std::to_string(pick(4)),
                           std::to_string(pick(50)));
                break;
              case 9:
                batch.HDel(bk, {"f" + std::to_string(pick(4))});
                break;
              case 10:
                batch.HIncr(bk, "f" + std::to_string(pick(4)), pick(21) - 10);
                break;
              case 0:
                batch.Set(bk, std::to_string(pick(100)));
                break;
              case 1:
                batch.Set(bk, std::to_string(pick(100)),
                          milliseconds(pick(60) - 5));
                break;
              case 2:
                batch.Del(bk);
                break;
              case 3:
                batch.Expire(bk, milliseconds(pick(60) - 5));
                break;
              case 4:
                batch.Persist(bk);
                break;
              case 5:
                batch.Append(bk, std::to_string(pick(10)));
                break;
              case 6:
                batch.Rename(bk, key());
                break;
              case 7:
                if (pick(2) == 0) {
                  batch.Incr(bk, pick(21) - 10);
                } else {
                  batch.Decr(bk, pick(21) - 10);
                }
                break;
            }
          }
          // The same batch must succeed or fail the same way on both.
          EXPECT_EQ(persistent->Write(batch).code(),
                    reference.Write(batch).code());
          break;
        }
        case 11:
          if (pick(10) == 0) {
            EXPECT_EQ(persistent->Keys(), reference.Keys());
            const std::string prefix = "k" + std::to_string(pick(8));
            EXPECT_EQ(persistent->Keys(prefix), reference.Keys(prefix));
          }
          break;
      }
    }

    persistent.reset();
    auto reopened = Open();  // same fake time: no restart
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->Keys(), reference.Keys()) << "seed=" << seed;
    for (int i = 0; i < 8; ++i) {
      const std::string k = "k" + std::to_string(i);
      EXPECT_EQ(Dump(*reopened, k), Dump(reference, k))
          << "seed=" << seed << " " << k;
      EXPECT_EQ(reopened->Get(k), reference.Get(k))
          << "seed=" << seed << " " << k;
      Db::TtlInfo a = reopened->GetTtl(k);
      Db::TtlInfo b = reference.GetTtl(k);
      EXPECT_EQ(a.state, b.state) << "seed=" << seed << " " << k;
      if (a.state == Db::TtlState::kExpiring) {
        EXPECT_EQ(a.remaining, b.remaining) << "seed=" << seed << " " << k;
      }
    }
  }
}

TEST_F(PersistTest, CompactShrinksLogToLiveData) {
  auto db = Open();
  for (int i = 0; i < 100; ++i)
    ASSERT_TRUE(db->Set("a", std::to_string(i)).ok());
  ASSERT_TRUE(db->Set("b", "1").ok());
  ASSERT_TRUE(*db->Del("b"));
  ASSERT_TRUE(db->Set("t", "v", seconds(5)).ok());
  ASSERT_TRUE(db->Set("gone", "x", seconds(1)).ok());
  Advance(seconds(2));  // "gone" expires, but is still in memory
  const uint64_t before = file_.Size();
  ASSERT_EQ(db->Size(), 3);

  ASSERT_TRUE(db->Compact().ok());
  EXPECT_LT(file_.Size(), before / 10);
  // Header, then "a" (28 bytes) and "t" (27 bytes). "gone" is dropped.
  EXPECT_EQ(file_.Size(), kLogHeaderSize + 28 + 27);
  EXPECT_EQ(db->Size(), 2);  // expired key swept from memory too

  ASSERT_TRUE(db->Set("after", "1").ok());  // appends to the new log
  db.reset();
  db = Open();
  EXPECT_EQ(db->recovery_info().records_replayed, 3);
  EXPECT_EQ(db->Get("a"), "99");
  EXPECT_EQ(db->Get("after"), "1");
  EXPECT_EQ(db->GetTtl("t").remaining, seconds(3));
}

TEST_F(PersistTest, AutoCompactionKeepsLogBounded) {
  OpenOptions o = Options();
  o.compact_min_bytes = 200;
  o.compact_ratio = 2;
  auto db = Open(o);
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(db->Set("k", std::to_string(i)).ok());
    ASSERT_LT(file_.Size(), 300) << "i=" << i;
  }
  EXPECT_GT(storage_->replace_count(), 0);
  db.reset();
  EXPECT_EQ(Open()->Get("k"), "199");
}

TEST_F(PersistTest, AutoCompactionCanBeDisabled) {
  OpenOptions o = Options();
  o.auto_compact = false;
  o.compact_min_bytes = 200;
  auto db = Open(o);
  for (int i = 0; i < 200; ++i)
    ASSERT_TRUE(db->Set("k", std::to_string(i)).ok());
  EXPECT_EQ(storage_->replace_attempts(), 0);
  EXPECT_GT(file_.Size(), 5000);
}

TEST_F(PersistTest, OpenCompactsAnOversizedLog) {
  {
    OpenOptions o = Options();
    o.auto_compact = false;
    auto db = Open(o);
    for (int i = 0; i < 200; ++i) {
      ASSERT_TRUE(db->Set("k", std::to_string(i)).ok());
    }
  }
  const uint64_t bloated = file_.Size();

  OpenOptions o = Options();
  o.compact_min_bytes = 100;
  auto db = Open(o);
  EXPECT_EQ(storage_->replace_count(), 1);
  EXPECT_LT(file_.Size(), bloated / 10);
  EXPECT_EQ(db->Get("k"), "199");

  db.reset();
  db = Open(o);  // already compact: nothing to do
  EXPECT_EQ(storage_->replace_attempts(), 0);
}

TEST_F(PersistTest, FailedCompactionLeavesOldLogInUse) {
  auto db = Open();
  ASSERT_TRUE(db->Set("a", "1").ok());
  ASSERT_TRUE(db->Set("a", "2").ok());
  const std::string before = file_.data();

  storage_->FailReplaceBegin(true);
  EXPECT_EQ(db->Compact().code(), StatusCode::kIoError);
  EXPECT_EQ(file_.data(), before);

  storage_->FailReplaceBegin(false);
  storage_->FailReplaceCommit(true);
  EXPECT_EQ(db->Compact().code(), StatusCode::kIoError);
  EXPECT_EQ(file_.data(), before);

  // The Db is still fully usable, and compaction works once the fault clears.
  ASSERT_TRUE(db->Set("b", "3").ok());
  storage_->FailReplaceCommit(false);
  ASSERT_TRUE(db->Compact().ok());
  db.reset();
  db = Open();
  EXPECT_EQ(db->Get("a"), "2");
  EXPECT_EQ(db->Get("b"), "3");
}

TEST_F(PersistTest, FailedAutoCompactionDoesNotFailWritesOrRetryEveryTime) {
  OpenOptions o = Options();
  o.compact_min_bytes = 1000;
  auto db = Open(o);
  storage_->FailReplaceBegin(true);
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Set("k", std::to_string(i)).ok()) << "i=" << i;
  }
  EXPECT_GE(storage_->replace_attempts(), 1);
  EXPECT_LT(storage_->replace_attempts(), 10);
  EXPECT_EQ(db->Get("k"), "99");
}

TEST_F(PersistTest, CompactOnInMemoryDbIsNoOp) {
  Db db;
  ASSERT_TRUE(db.Set("a", "1").ok());
  EXPECT_TRUE(db.Compact().ok());
  EXPECT_EQ(db.Get("a"), "1");
}

// The payload of the newest entry.
std::string Payload(Db& db, const std::string& key) {
  return db.XRevRange(key, 1)->at(0).payload;
}

// A stream's text as Dump() shows it, for the entries and last ID given.
std::string StreamText(const StreamId& last,
                       const std::vector<std::pair<StreamId, std::string>>& e) {
  std::string out = "stream:last=" + last.ToString() + ";";
  for (const auto& [id, payload] : e)
    out += id.ToString() + "=" + payload + ",";
  return out;
}

TEST_F(PersistTest, StreamsSurviveReopenWithTheirIdsAndTtl) {
  std::string before;
  StreamId last;
  {
    auto db = Open();
    ASSERT_TRUE(db->XAdd("s", "a").ok());
    Advance(milliseconds(5));
    ASSERT_TRUE(db->XAdd("s", "b").ok());
    ASSERT_TRUE(db->XAdd("s", std::string("\0bin\n", 5)).ok());
    ASSERT_EQ(*db->XTrimMinId("s", StreamId{1700000000005, 0}), 1);
    ASSERT_TRUE(*db->Expire("s", seconds(100)));
    ASSERT_TRUE(db->XAdd("gone", "x", 0).ok());
    ASSERT_TRUE(*db->Del("gone"));
    last = **db->XLastId("s");
    before = Dump(*db, "s");
  }
  Restart(seconds(40));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "s"), before);
  EXPECT_EQ(db->GetTtl("s").remaining, seconds(60));
  EXPECT_EQ(db->Type("gone"), KeyType::kNone);
  StatusOr<StreamId> next = db->XAdd("s", "after");
  ASSERT_TRUE(next.ok());
  EXPECT_GT(*next, last);
}

TEST_F(PersistTest, AnEmptiedStreamKeepsItsLastIdThroughReopenAndCompaction) {
  StreamId last;
  {
    auto db = Open();
    ASSERT_TRUE(db->XAdd("s", "a").ok());
    StatusOr<StreamId> id = db->XAdd("s", "b", 0);  // trims everything
    ASSERT_TRUE(id.ok());
    last = *id;
    ASSERT_EQ(*db->XLen("s"), 0);
  }
  for (int round = 0; round < 3; ++round) {
    {
      auto db = Open();
      EXPECT_EQ(Dump(*db, "s"), StreamText(last, {})) << round;
      EXPECT_EQ(db->Type("s"), KeyType::kStream);
      ASSERT_TRUE(db->Compact().ok());  // and compaction keeps it too
    }
    Restart(seconds(0));
  }
  // Even with the clock far behind, the next ID is still greater.
  Restart(milliseconds(-100000));
  auto db = Open();
  StatusOr<StreamId> next = db->XAdd("s", "c");
  ASSERT_TRUE(next.ok());
  EXPECT_EQ(*next, (StreamId{last.ms, last.seq + 1}));
}

TEST_F(PersistTest, TheClockGoingBackwardsAcrossARestartKeepsIdsGrowing) {
  StreamId last;
  {
    auto db = Open();
    last = *db->XAdd("s", "a");
  }
  Restart(milliseconds(-5000));
  auto db = Open();
  EXPECT_EQ(*db->XAdd("s", "b"), (StreamId{last.ms, last.seq + 1}));
}

TEST_F(PersistTest, CompactionKeepsAStreamInPiecesThatFitARecord) {
  std::vector<std::pair<StreamId, std::string>> expected;
  std::string before;
  StreamId last;
  {
    auto db = Open(SmallChunks(Options()));
    for (int i = 0; i < 300; ++i) {
      if (i % 7 == 0) Advance(milliseconds(1));
      std::string payload = std::to_string(i) + std::string(90, 'x');
      StatusOr<StreamId> id = db->XAdd("big", payload);
      ASSERT_TRUE(id.ok());
      expected.emplace_back(*id, payload);
      last = *id;
    }
    ASSERT_EQ(*db->XTrimMaxLen("big", 250), 50);
    expected.erase(expected.begin(), expected.begin() + 50);
    ASSERT_TRUE(*db->Expire("big", seconds(500)));
    ASSERT_TRUE(db->XAdd("small", "v").ok());
    ASSERT_TRUE(db->Compact().ok());
    EXPECT_EQ(storage_->replace_count(), 1);
  }
  Restart(seconds(100));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "big"), StreamText(last, expected));
  EXPECT_EQ(db->GetTtl("big").remaining, seconds(400));
  // The compacted log holds one record for each entry of the big stream.
  EXPECT_GE(db->recovery_info().records_replayed, 250);
}

TEST_F(PersistTest, AnAddWithAMaxLenIsAllOrNothingWhateverByteACrashStopsAt) {
  StreamId first;
  uint64_t before;
  std::string after_bytes;
  {
    auto db = Open();
    first = *db->XAdd("s", "a", 1);
    before = file_.Size();
    ASSERT_TRUE(db->XAdd("s", "b", 1).ok());  // the add and the trim, as one
    after_bytes = file_.data();
  }
  Restart(seconds(0));
  ASSERT_GT(after_bytes.size(), before);
  for (uint64_t cut = before; cut <= after_bytes.size(); ++cut) {
    file_.data() = after_bytes.substr(0, cut);
    auto db = Open();
    ASSERT_NE(db, nullptr);
    // Never both entries, and never none.
    if (cut < after_bytes.size()) {
      EXPECT_EQ(Dump(*db, "s"), StreamText(first, {{first, "a"}})) << cut;
    } else {
      EXPECT_EQ(*db->XLen("s"), 1);
      EXPECT_EQ(Payload(*db, "s"), "b");
    }
  }
}

TEST_F(PersistTest, ANewStreamDoesNotInheritAStaleExpiredEntryAfterRestart) {
  // "s" expires without anyone touching it, so the log still holds its
  // entries and deadline. Creating a stream under the name afterwards has to
  // start from nothing, or recovery would merge into the stale one.
  std::string live;
  {
    auto db = Open();
    ASSERT_TRUE(db->XAdd("s", "old1").ok());
    ASSERT_TRUE(db->XAdd("s", "old2").ok());
    ASSERT_TRUE(*db->Expire("s", seconds(10)));
    ASSERT_TRUE(db->Set("t", "string", seconds(10)).ok());
    Advance(seconds(10));  // both expire, unnoticed

    ASSERT_TRUE(db->XAdd("s", "new").ok());
    ASSERT_TRUE(db->XAdd("t", "new").ok());  // over a stale string
    live = Dump(*db, "s");
    ASSERT_EQ(*db->XLen("s"), 1);
  }
  Restart(seconds(0));
  auto db = Open();
  EXPECT_EQ(Dump(*db, "s"), live);
  EXPECT_EQ(*db->XLen("t"), 1);
  for (const char* k : {"s", "t"}) {
    EXPECT_EQ(db->GetTtl(k).state, Db::TtlState::kNoExpiry) << k;
  }
}

TEST_F(PersistTest, ReplayIgnoresAnAddThatIsNotNewerThanTheStream) {
  auto add = [](const char* key, const char* payload, int64_t fresh, int64_t ms,
                int64_t seq) {
    Record r;
    r.type = Record::Type::kXAdd;
    r.key = key;
    r.items = {payload};
    r.n1 = fresh;
    r.n2 = ms;
    r.n3 = seq;
    return r;
  };
  ASSERT_TRUE(WriteHeader(file_).ok());
  for (const Record& r : {add("s", "a", 1, 5, 1), add("s", "same", 0, 5, 1),
                          add("s", "older", 0, 5, 0), add("s", "b", 0, 5, 2),
                          add("z", "zero", 1, 0, 0)}) {
    ASSERT_TRUE(AppendRecord(file_, r).ok());
  }
  auto db = Open();
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(Dump(*db, "s"), StreamText({5, 2}, {{{5, 1}, "a"}, {{5, 2}, "b"}}));
  EXPECT_EQ(db->Type("z"), KeyType::kNone);  // nothing came of 0-0
}

TEST(DbFileTest, SecondOpenOfSamePathIsRefused) {
  const std::string path = testing::TempDir() + "/db_lock_test.qey";
  std::remove(path.c_str());
  {
    StatusOr<std::unique_ptr<Db>> first = Db::Open(path);
    ASSERT_TRUE(first.ok()) << first.status().message();
    StatusOr<std::unique_ptr<Db>> second = Db::Open(path);
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), StatusCode::kUnavailable);
  }
  EXPECT_TRUE(Db::Open(path).ok());  // released when the first Db closed
  std::remove(path.c_str());
}

TEST(DbFileTest, CompactionOnRealFileKeepsDataAndLock) {
  const std::string path = testing::TempDir() + "/db_compact_test.qey";
  std::remove(path.c_str());
  {
    StatusOr<std::unique_ptr<Db>> db = Db::Open(path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    for (int i = 0; i < 500; ++i) {
      ASSERT_TRUE((*db)->Set("k", std::to_string(i)).ok());
    }
    const auto before = std::filesystem::file_size(path);
    ASSERT_TRUE((*db)->Compact().ok());
    EXPECT_LT(std::filesystem::file_size(path), before / 10);
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));

    // Still locked after the file was swapped, and still writable.
    EXPECT_EQ(Db::Open(path).status().code(), StatusCode::kUnavailable);
    ASSERT_TRUE((*db)->Set("after", "compaction").ok());
  }
  StatusOr<std::unique_ptr<Db>> db = Db::Open(path);
  ASSERT_TRUE(db.ok()) << db.status().message();
  EXPECT_EQ((*db)->Get("k"), "499");
  EXPECT_EQ((*db)->Get("after"), "compaction");
  EXPECT_EQ((*db)->recovery_info().records_replayed, 2);
  db = StatusOr<std::unique_ptr<Db>>(Status(StatusCode::kIoError, "closed"));
  std::remove(path.c_str());
}

TEST(DbFileTest, OpensRealFileAndPersistsAcrossReopen) {
  const std::string path = testing::TempDir() + "/db_file_test.qey";
  std::remove(path.c_str());
  {
    StatusOr<std::unique_ptr<Db>> db = Db::Open(path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    ASSERT_TRUE((*db)->Set("hello", "world").ok());
    ASSERT_TRUE((*db)->Set("n", "41").ok());
    ASSERT_EQ(*(*db)->Incr("n"), 42);
    ASSERT_TRUE((*db)->Set("temp", "x", seconds(3600)).ok());
  }
  {
    StatusOr<std::unique_ptr<Db>> db = Db::Open(path);
    ASSERT_TRUE(db.ok()) << db.status().message();
    EXPECT_EQ((*db)->Get("hello"), "world");
    EXPECT_EQ((*db)->Get("n"), "42");
    EXPECT_EQ((*db)->GetTtl("temp").state, Db::TtlState::kExpiring);
    EXPECT_EQ((*db)->recovery_info().records_replayed, 4);
  }
  std::remove(path.c_str());
}

TEST(DbFileTest, OpenFailsOnDirectory) {
  StatusOr<std::unique_ptr<Db>> db = Db::Open(testing::TempDir());
  EXPECT_FALSE(db.ok());
}

}  // namespace
}  // namespace qey
