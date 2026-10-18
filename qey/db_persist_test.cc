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
    Db reference([this] { return steady_; });
    ASSERT_NE(persistent, nullptr);

    for (int i = 0; i < 2000; ++i) {
      const std::string k = key();
      switch (pick(12)) {
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
          StatusOr<size_t> a = persistent->Append(k, suffix);
          StatusOr<size_t> b = reference.Append(k, suffix);
          ASSERT_TRUE(a.ok() && b.ok());
          EXPECT_EQ(*a, *b);
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
