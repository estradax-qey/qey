#include "qey/db.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace qey {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

class DbTest : public testing::Test {
 protected:
  DbTest() : db_([this] { return now_; }) {}

  void Advance(milliseconds d) { now_ += d; }

  // Wrappers that check the call succeeded, so tests can focus on behavior.
  void Set(std::string_view key, std::string_view value,
           std::optional<milliseconds> ttl = std::nullopt) {
    EXPECT_TRUE(db_.Set(key, value, ttl).ok());
  }
  bool Del(std::string_view key) { return Unwrap(db_.Del(key)); }
  bool Expire(std::string_view key, milliseconds ttl) {
    return Unwrap(db_.Expire(key, ttl));
  }
  bool Persist(std::string_view key) { return Unwrap(db_.Persist(key)); }

  static bool Unwrap(const StatusOr<bool>& r) {
    EXPECT_TRUE(r.ok());
    return r.ok() && *r;
  }

  Db::Clock::time_point now_ = Db::Clock::time_point(milliseconds(1000));
  Db db_;
};

TEST_F(DbTest, GetMissingKeyReturnsNullopt) {
  EXPECT_EQ(db_.Get("missing"), std::nullopt);
}

TEST_F(DbTest, SetThenGet) {
  Set("k", "v");
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, SetOverwritesValue) {
  Set("k", "one");
  Set("k", "two");
  EXPECT_EQ(db_.Get("k"), "two");
}

TEST_F(DbTest, ValuesMayBeEmptyAndContainNul) {
  Set("empty", "");
  Set("bin", std::string_view("a\0b", 3));
  EXPECT_EQ(db_.Get("empty"), "");
  EXPECT_EQ(db_.Get("bin"), std::string("a\0b", 3));
}

TEST_F(DbTest, DelRemovesKey) {
  Set("k", "v");
  EXPECT_TRUE(Del("k"));
  EXPECT_EQ(db_.Get("k"), std::nullopt);
}

TEST_F(DbTest, DelMissingKeyReturnsFalse) { EXPECT_FALSE(Del("nope")); }

TEST_F(DbTest, KeyIsLiveBeforeTtlElapses) {
  Set("k", "v", milliseconds(100));
  Advance(milliseconds(99));
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, KeyExpiresWhenTtlElapses) {
  Set("k", "v", milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Get("k"), std::nullopt);
}

TEST_F(DbTest, DelOnExpiredKeyReturnsFalse) {
  Set("k", "v", milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_FALSE(Del("k"));
}

TEST_F(DbTest, SetWithoutTtlClearsPreviousTtl) {
  Set("k", "v", milliseconds(100));
  Set("k", "v2");
  Advance(milliseconds(1000));
  EXPECT_EQ(db_.Get("k"), "v2");
}

TEST_F(DbTest, ExistsReflectsLiveKeys) {
  EXPECT_FALSE(db_.Exists("k"));
  Set("k", "v", milliseconds(100));
  EXPECT_TRUE(db_.Exists("k"));
  Advance(milliseconds(100));
  EXPECT_FALSE(db_.Exists("k"));
}

TEST_F(DbTest, ExpireSetsTtlOnExistingKey) {
  Set("k", "v");
  EXPECT_TRUE(Expire("k", milliseconds(50)));
  Advance(milliseconds(50));
  EXPECT_EQ(db_.Get("k"), std::nullopt);
}

TEST_F(DbTest, ExpireReplacesExistingTtl) {
  Set("k", "v", milliseconds(50));
  EXPECT_TRUE(Expire("k", milliseconds(500)));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, ExpireMissingKeyReturnsFalse) {
  EXPECT_FALSE(Expire("nope", milliseconds(50)));
}

TEST_F(DbTest, ExpireWithNonPositiveTtlExpiresImmediately) {
  Set("k", "v");
  EXPECT_TRUE(Expire("k", milliseconds(0)));
  EXPECT_FALSE(db_.Exists("k"));
}

TEST_F(DbTest, PersistRemovesTtl) {
  Set("k", "v", milliseconds(50));
  EXPECT_TRUE(Persist("k"));
  Advance(milliseconds(1000));
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, PersistReturnsFalseWithoutTtlOrKey) {
  Set("k", "v");
  EXPECT_FALSE(Persist("k"));
  EXPECT_FALSE(Persist("nope"));
}

TEST_F(DbTest, GetTtlReportsState) {
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kMissing);

  Set("k", "v");
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kNoExpiry);

  Expire("k", milliseconds(100));
  Advance(milliseconds(30));
  Db::TtlInfo info = db_.GetTtl("k");
  EXPECT_EQ(info.state, Db::TtlState::kExpiring);
  EXPECT_EQ(info.remaining, milliseconds(70));

  Advance(milliseconds(70));
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kMissing);
}

TEST_F(DbTest, IncrOnMissingKeyStartsAtZero) {
  StatusOr<int64_t> r = db_.Incr("n");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, 1);
  EXPECT_EQ(db_.Get("n"), "1");
}

TEST_F(DbTest, IncrAndDecrApplyDelta) {
  Set("n", "10");
  EXPECT_EQ(*db_.Incr("n", 5), 15);
  EXPECT_EQ(*db_.Decr("n", 20), -5);
  EXPECT_EQ(*db_.Decr("n"), -6);
  EXPECT_EQ(db_.Get("n"), "-6");
}

TEST_F(DbTest, IncrPreservesTtl) {
  Set("n", "1", milliseconds(100));
  ASSERT_TRUE(db_.Incr("n").ok());
  Advance(milliseconds(100));
  EXPECT_FALSE(db_.Exists("n"));
}

TEST_F(DbTest, IncrOnExpiredKeyRestartsWithoutTtl) {
  Set("n", "41", milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_EQ(*db_.Incr("n"), 1);
  EXPECT_EQ(db_.GetTtl("n").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, IncrRejectsNonIntegerValues) {
  for (const char* bad : {"", "abc", "1.5", " 1", "1 ", "+1", "0x10"}) {
    Set("n", bad);
    StatusOr<int64_t> r = db_.Incr("n");
    EXPECT_FALSE(r.ok()) << bad;
    EXPECT_EQ(r.status().code(), StatusCode::kNotInteger) << bad;
    EXPECT_EQ(db_.Get("n"), bad);
  }
}

TEST_F(DbTest, IncrRejectsValueOutsideInt64) {
  Set("n", "99999999999999999999");
  EXPECT_EQ(db_.Incr("n").status().code(), StatusCode::kNotInteger);
}

TEST_F(DbTest, IncrDetectsOverflow) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  Set("n", std::to_string(kMax));
  StatusOr<int64_t> r = db_.Incr("n");
  EXPECT_EQ(r.status().code(), StatusCode::kOutOfRange);
  EXPECT_EQ(db_.Get("n"), std::to_string(kMax));
}

TEST_F(DbTest, DecrDetectsUnderflowAndHandlesInt64Min) {
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  Set("n", std::to_string(kMin));
  EXPECT_EQ(db_.Decr("n").status().code(), StatusCode::kOutOfRange);

  Set("n", "-1");
  StatusOr<int64_t> r = db_.Decr("n", kMin);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, std::numeric_limits<int64_t>::max());
}

TEST_F(DbTest, SweepRemovesOnlyExpiredKeys) {
  Set("a", "1", milliseconds(10));
  Set("b", "2", milliseconds(20));
  Set("c", "3");
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Size(), 2);
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Size(), 1);
  EXPECT_EQ(db_.Get("c"), "3");
}

TEST_F(DbTest, SizeCountsUnsweptExpiredKeys) {
  Set("a", "1", milliseconds(10));
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Size(), 1);
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Size(), 0);
}

TEST_F(DbTest, SweepHonorsLimit) {
  for (const char* k : {"a", "b", "c"}) Set(k, "v", milliseconds(10));
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Sweep(2), 2);
  EXPECT_EQ(db_.Size(), 1);
  EXPECT_EQ(db_.Sweep(2), 1);
  EXPECT_EQ(db_.Size(), 0);
}

TEST_F(DbTest, SweepIgnoresKeysWhoseExpiryChanged) {
  Set("a", "1", milliseconds(10));
  Set("b", "2", milliseconds(10));
  Set("c", "3", milliseconds(10));
  Expire("a", milliseconds(1000));
  Persist("b");
  Set("c", "3");
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Sweep(), 0);
  EXPECT_EQ(db_.Size(), 3);
}

TEST_F(DbTest, SweepAfterDelDoesNotTouchOtherKeys) {
  Set("a", "1", milliseconds(10));
  Set("b", "2", milliseconds(10));
  Del("a");
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Size(), 0);
}

TEST_F(DbTest, SetRejectsTtlBeyondMaxAndChangesNothing) {
  Set("k", "old");
  for (milliseconds ttl :
       {Db::kMaxTtl + milliseconds(1), milliseconds::max()}) {
    EXPECT_EQ(db_.Set("k", "new", ttl).code(), StatusCode::kInvalidArgument);
  }
  EXPECT_EQ(db_.Get("k"), "old");
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kNoExpiry);
  EXPECT_EQ(db_.Set("fresh", "v", milliseconds::max()).code(),
            StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("fresh"));
}

TEST_F(DbTest, ExpireRejectsTtlBeyondMaxAndChangesNothing) {
  Set("k", "v", milliseconds(500));
  EXPECT_EQ(db_.Expire("k", milliseconds::max()).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.Expire("k", Db::kMaxTtl + milliseconds(1)).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.GetTtl("k").remaining, milliseconds(500));
  // A missing key is still an argument error, not "false".
  EXPECT_FALSE(db_.Expire("nope", milliseconds::max()).ok());
}

TEST_F(DbTest, MaxTtlIsAccepted) {
  Set("k", "v", Db::kMaxTtl);
  Db::TtlInfo info = db_.GetTtl("k");
  EXPECT_EQ(info.state, Db::TtlState::kExpiring);
  EXPECT_EQ(info.remaining, Db::kMaxTtl);
  EXPECT_TRUE(Expire("k", Db::kMaxTtl));
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, VeryNegativeTtlExpiresImmediately) {
  Set("a", "v", milliseconds::min());
  EXPECT_FALSE(db_.Exists("a"));
  Set("b", "v");
  EXPECT_TRUE(Expire("b", milliseconds::min()));
  EXPECT_FALSE(db_.Exists("b"));
}

using Keys = std::vector<std::string>;

TEST_F(DbTest, AppendCreatesAndExtends) {
  EXPECT_EQ(*db_.Append("k", "ab"), 2);
  EXPECT_EQ(*db_.Append("k", "cd"), 4);
  EXPECT_EQ(db_.Get("k"), "abcd");

  EXPECT_EQ(*db_.Append("empty", ""), 0);  // still creates the key
  EXPECT_EQ(db_.Get("empty"), "");

  EXPECT_EQ(*db_.Append("bin", std::string_view("\0x", 2)), 2);
  EXPECT_EQ(*db_.Append("bin", std::string_view("\0", 1)), 3);
  EXPECT_EQ(db_.Get("bin"), std::string("\0x\0", 3));
}

TEST_F(DbTest, AppendKeepsTtl) {
  Set("k", "ab", milliseconds(100));
  Advance(milliseconds(40));
  EXPECT_EQ(*db_.Append("k", "cd"), 4);
  EXPECT_EQ(db_.GetTtl("k").remaining, milliseconds(60));
  Advance(milliseconds(60));
  EXPECT_FALSE(db_.Exists("k"));
}

TEST_F(DbTest, AppendToExpiredKeyStartsFresh) {
  Set("k", "old", milliseconds(10));
  Advance(milliseconds(10));
  EXPECT_EQ(*db_.Append("k", "new"), 3);
  EXPECT_EQ(db_.Get("k"), "new");
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, OversizedKeysAreRejectedEverywhere) {
  const std::string big_key(Db::kMaxKeySize + 1, 'k');
  const std::string ok_key(Db::kMaxKeySize, 'k');
  EXPECT_EQ(db_.Set(big_key, "v").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.Append(big_key, "v").status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.Incr(big_key).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.Decr(big_key).status().code(), StatusCode::kInvalidArgument);
  Set("a", "v");
  EXPECT_EQ(db_.Rename("a", big_key).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.Get("a"), "v");
  EXPECT_EQ(db_.Size(), 1);

  Set(ok_key, "v");  // exactly at the limit is fine
  EXPECT_EQ(db_.Get(ok_key), "v");
}

TEST_F(DbTest, ValueSizeLimit) {
  std::string big(Db::kMaxValueSize + 1, 'x');
  const std::string_view at_limit(big.data(), Db::kMaxValueSize);
  EXPECT_EQ(db_.Set("k", big).code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("k"));

  ASSERT_TRUE(db_.Set("k", at_limit).ok());
  EXPECT_EQ(db_.Append("k", "y").status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.Append("fresh", big).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("fresh"));
  EXPECT_EQ(*db_.Append("k", ""), Db::kMaxValueSize);  // adding nothing is fine
}

TEST_F(DbTest, RenameMovesValueAndTtl) {
  Set("a", "va", milliseconds(100));
  EXPECT_TRUE(*db_.Rename("a", "b"));
  EXPECT_FALSE(db_.Exists("a"));
  EXPECT_EQ(db_.Get("b"), "va");
  EXPECT_EQ(db_.GetTtl("b").remaining, milliseconds(100));
  EXPECT_EQ(db_.Size(), 1);

  // The expiry index followed the key: sweeping removes it under its new name.
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Size(), 0);
}

TEST_F(DbTest, RenameReplacesDestinationAndItsTtl) {
  Set("a", "va");
  Set("b", "vb", milliseconds(50));
  EXPECT_TRUE(*db_.Rename("a", "b"));
  EXPECT_EQ(db_.Get("b"), "va");
  EXPECT_EQ(db_.GetTtl("b").state, Db::TtlState::kNoExpiry);
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Sweep(), 0);  // the old TTL on "b" is gone
  EXPECT_EQ(db_.Get("b"), "va");
}

TEST_F(DbTest, RenameReplacesAnExpiredDestination) {
  Set("b", "vb", milliseconds(10));
  Set("a", "va");
  Advance(milliseconds(10));
  EXPECT_TRUE(*db_.Rename("a", "b"));
  EXPECT_EQ(db_.Get("b"), "va");
  EXPECT_EQ(db_.Size(), 1);
}

TEST_F(DbTest, RenameOfMissingOrExpiredSourceReturnsFalse) {
  EXPECT_FALSE(*db_.Rename("nope", "b"));
  Set("a", "v", milliseconds(10));
  Set("b", "keep");
  Advance(milliseconds(10));
  EXPECT_FALSE(*db_.Rename("a", "b"));
  EXPECT_EQ(db_.Get("b"), "keep");
}

TEST_F(DbTest, RenameToSameNameIsANoOp) {
  Set("a", "v", milliseconds(100));
  EXPECT_TRUE(*db_.Rename("a", "a"));
  EXPECT_EQ(db_.Get("a"), "v");
  EXPECT_EQ(db_.GetTtl("a").remaining, milliseconds(100));
  EXPECT_FALSE(*db_.Rename("nope", "nope"));
}

TEST_F(DbTest, ClearRemovesEverything) {
  Set("a", "1");
  Set("b", "2", milliseconds(10));
  ASSERT_TRUE(db_.Clear().ok());
  EXPECT_EQ(db_.Size(), 0);
  EXPECT_FALSE(db_.Exists("a"));
  EXPECT_EQ(db_.Keys(), Keys{});
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Sweep(), 0);  // the expiry index was cleared too

  Set("a", "again");  // the Db is fully usable afterwards
  EXPECT_EQ(db_.Get("a"), "again");
  EXPECT_EQ(db_.Keys(), Keys{"a"});
  EXPECT_TRUE(db_.Clear().ok());  // clearing an empty Db is fine
}

TEST_F(DbTest, KeysAreSortedAndFilteredByPrefix) {
  for (const char* k : {"b2", "a1", "b1", "c", "a", "", "b"}) Set(k, "v");
  EXPECT_EQ(db_.Keys(), (Keys{"", "a", "a1", "b", "b1", "b2", "c"}));
  EXPECT_EQ(db_.Keys(""), db_.Keys());
  EXPECT_EQ(db_.Keys("b"), (Keys{"b", "b1", "b2"}));
  EXPECT_EQ(db_.Keys("b1"), (Keys{"b1"}));
  EXPECT_EQ(db_.Keys("a1x"), Keys{});
  EXPECT_EQ(db_.Keys("zzz"), Keys{});

  Set(std::string_view("\0a", 2), "v");
  Set(std::string_view("\xff\xff", 2), "v");
  EXPECT_EQ(db_.Keys(std::string_view("\0", 1)), Keys{std::string("\0a", 2)});
  EXPECT_EQ(db_.Keys(std::string_view("\xff", 1)),
            Keys{std::string("\xff\xff", 2)});
}

TEST_F(DbTest, KeysSkipExpiredKeysThatAreNotSweptYet) {
  Set("a", "1", milliseconds(10));
  Set("b", "2");
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Keys(), Keys{"b"});
  EXPECT_EQ(db_.Size(), 2);
}

TEST_F(DbTest, ScanPagesThroughAllMatchingKeys) {
  for (int i = 0; i < 10; ++i) Set("k0" + std::to_string(i), "v");
  Set("other", "v");

  Keys seen;
  std::vector<size_t> page_sizes;
  std::optional<std::string> cursor;
  do {
    ScanPage page = db_.Scan("k", 3, cursor);
    page_sizes.push_back(page.keys.size());
    seen.insert(seen.end(), page.keys.begin(), page.keys.end());
    cursor = page.next;
  } while (cursor.has_value());
  EXPECT_EQ(seen, db_.Keys("k"));
  EXPECT_EQ(page_sizes, (std::vector<size_t>{3, 3, 3, 1}));
}

TEST_F(DbTest, ScanEndsExactlyWhenNothingIsLeft) {
  for (int i = 0; i < 10; ++i) Set("k0" + std::to_string(i), "v");
  ScanPage first = db_.Scan("k", 5);
  ASSERT_EQ(first.keys.size(), 5);
  ASSERT_TRUE(first.next.has_value());
  ScanPage second = db_.Scan("k", 5, first.next);
  EXPECT_EQ(second.keys.size(), 5);
  EXPECT_FALSE(second.next.has_value());  // no empty trailing page

  EXPECT_FALSE(db_.Scan("k", 100).next.has_value());
  ScanPage none = db_.Scan("nope", 5);
  EXPECT_TRUE(none.keys.empty());
  EXPECT_FALSE(none.next.has_value());
}

TEST_F(DbTest, ScanIncludesTheEmptyKeyAndTreatsZeroLimitAsOne) {
  Set("", "v");
  Set("a", "v");
  ScanPage page = db_.Scan("", 0);
  EXPECT_EQ(page.keys, Keys{""});
  ASSERT_TRUE(page.next.has_value());
  EXPECT_EQ(db_.Scan("", 0, page.next).keys, Keys{"a"});
}

TEST_F(DbTest, ScanWithCursorBeforeThePrefixStartsAtThePrefix) {
  for (const char* k : {"a", "m1", "m2", "z"}) Set(k, "v");
  EXPECT_EQ(db_.Scan("m", 10, std::string("a")).keys, (Keys{"m1", "m2"}));
  EXPECT_EQ(db_.Scan("m", 10, std::string("z")).keys, Keys{});
}

TEST_F(DbTest, ScanReturnsEachStableKeyOnceDespiteChanges) {
  for (int i = 0; i < 10; ++i) Set("k0" + std::to_string(i), "v");
  ScanPage page = db_.Scan("k", 4);
  ASSERT_EQ(page.keys, (Keys{"k00", "k01", "k02", "k03"}));

  EXPECT_TRUE(*db_.Del("k01"));  // behind the cursor
  EXPECT_TRUE(*db_.Del("k05"));  // ahead of it
  Set("k035", "v");              // lands just ahead of the cursor
  Set("k99", "v");
  Set("a-before", "v");  // outside the prefix

  Keys rest;
  for (auto cursor = page.next; cursor.has_value();) {
    ScanPage next = db_.Scan("k", 4, cursor);
    rest.insert(rest.end(), next.keys.begin(), next.keys.end());
    cursor = next.next;
  }
  EXPECT_EQ(rest, (Keys{"k035", "k04", "k06", "k07", "k08", "k09", "k99"}));
}

TEST_F(DbTest, KeyIndexStaysCurrentAfterItIsBuilt) {
  Set("a", "1");
  Set("b", "2");
  ASSERT_EQ(db_.Keys(), (Keys{"a", "b"}));  // builds the index

  Set("c", "3");
  EXPECT_EQ(db_.Keys(), (Keys{"a", "b", "c"}));
  EXPECT_TRUE(*db_.Del("a"));
  EXPECT_EQ(db_.Keys(), (Keys{"b", "c"}));
  EXPECT_TRUE(*db_.Rename("b", "z"));
  EXPECT_EQ(db_.Keys(), (Keys{"c", "z"}));
  EXPECT_EQ(*db_.Append("new", "x"), 1);
  EXPECT_EQ(*db_.Incr("n"), 1);
  EXPECT_EQ(db_.Keys(), (Keys{"c", "n", "new", "z"}));

  Set("t", "v", milliseconds(10));
  Advance(milliseconds(10));
  EXPECT_EQ(db_.Sweep(), 1);  // erasing an expired key updates the index
  EXPECT_EQ(db_.Keys(), (Keys{"c", "n", "new", "z"}));

  ASSERT_TRUE(db_.Clear().ok());
  EXPECT_EQ(db_.Keys(), Keys{});
  Set("again", "v");
  EXPECT_EQ(db_.Keys(), Keys{"again"});
}

TEST_F(DbTest, BatchAppliesChangesInOrderEachSeeingTheLast) {
  WriteBatch batch;
  batch.Set("a", "1")
      .Set("b", "2")
      .Rename("a", "c")  // sees the Set above
      .Append("c", "x")
      .Del("b")
      .Incr("n", 5)
      .Incr("n")
      .Decr("m", 3)
      .Set("t", "v", milliseconds(100));
  ASSERT_TRUE(db_.Write(batch).ok());

  EXPECT_FALSE(db_.Exists("a"));
  EXPECT_FALSE(db_.Exists("b"));
  EXPECT_EQ(db_.Get("c"), "1x");
  EXPECT_EQ(db_.Get("n"), "6");
  EXPECT_EQ(db_.Get("m"), "-3");
  EXPECT_EQ(db_.GetTtl("t").remaining, milliseconds(100));
  EXPECT_EQ(db_.Keys(), (Keys{"c", "m", "n", "t"}));
}

TEST_F(DbTest, BatchLastChangeToAKeyWins) {
  Set("k", "old");
  WriteBatch batch;
  batch.Set("k", "one")
      .Del("k")
      .Set("k", "two", milliseconds(50))
      .Set("k", "3");
  ASSERT_TRUE(db_.Write(batch).ok());
  EXPECT_EQ(db_.Get("k"), "3");
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, BatchExpireAndPersist) {
  WriteBatch batch;
  batch.Set("a", "1", milliseconds(50))
      .Persist("a")
      .Set("b", "2")
      .Expire("b", milliseconds(50));
  ASSERT_TRUE(db_.Write(batch).ok());
  Advance(milliseconds(50));
  EXPECT_TRUE(db_.Exists("a"));
  EXPECT_FALSE(db_.Exists("b"));
}

TEST_F(DbTest, BatchChangesToMissingKeysAreNoOps) {
  WriteBatch batch;
  batch.Del("x").Expire("x", milliseconds(5)).Persist("x").Rename("x", "y");
  ASSERT_TRUE(db_.Write(batch).ok());
  EXPECT_EQ(db_.Size(), 0);
}

TEST_F(DbTest, EmptyBatchSucceedsAndABatchCanBeWrittenAgain) {
  WriteBatch batch;
  EXPECT_TRUE(batch.empty());
  EXPECT_TRUE(db_.Write(batch).ok());

  batch.Incr("n").Append("log", "x");
  EXPECT_EQ(batch.size(), 2);
  ASSERT_TRUE(db_.Write(batch).ok());
  ASSERT_TRUE(db_.Write(batch).ok());
  EXPECT_EQ(db_.Get("n"), "2");
  EXPECT_EQ(db_.Get("log"), "xx");

  batch.Clear();
  EXPECT_TRUE(batch.empty());
  EXPECT_TRUE(db_.Write(batch).ok());
  EXPECT_EQ(db_.Get("n"), "2");
}

TEST_F(DbTest, FailedBatchChangesNothing) {
  Set("a", "va", milliseconds(100));
  Set("b", "vb");
  Set("text", "abc");
  Set("dest", "old", milliseconds(30));
  ASSERT_EQ(db_.Keys().size(), 4);  // builds the key index

  WriteBatch batch;
  batch.Set("new", "x")
      .Set("a", "overwritten")
      .Del("b")
      .Rename("a", "dest")
      .Expire("text", milliseconds(1))
      .Set("later", "y", milliseconds(10))
      .Incr("text");  // not an integer: fails the whole batch
  EXPECT_EQ(db_.Write(batch).code(), StatusCode::kNotInteger);

  EXPECT_EQ(db_.Keys(), (Keys{"a", "b", "dest", "text"}));
  EXPECT_EQ(db_.Size(), 4);
  EXPECT_EQ(db_.Get("a"), "va");
  EXPECT_EQ(db_.GetTtl("a").remaining, milliseconds(100));
  EXPECT_EQ(db_.Get("b"), "vb");
  EXPECT_EQ(db_.GetTtl("b").state, Db::TtlState::kNoExpiry);
  EXPECT_EQ(db_.Get("dest"), "old");
  EXPECT_EQ(db_.GetTtl("dest").remaining, milliseconds(30));
  EXPECT_EQ(db_.GetTtl("text").state, Db::TtlState::kNoExpiry);
  EXPECT_FALSE(db_.Exists("new"));
  EXPECT_FALSE(db_.Exists("later"));

  // The expiry index was restored too: exactly "dest" then "a" expire.
  Advance(milliseconds(30));
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Keys(), (Keys{"a", "b", "text"}));
  Advance(milliseconds(70));
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Keys(), (Keys{"b", "text"}));
}

TEST_F(DbTest, BatchOverflowAbortsEverything) {
  Set("n", std::to_string(std::numeric_limits<int64_t>::max()));
  WriteBatch batch;
  batch.Set("x", "1").Incr("n");
  EXPECT_EQ(db_.Write(batch).code(), StatusCode::kOutOfRange);
  EXPECT_FALSE(db_.Exists("x"));

  WriteBatch under;
  Set("m", std::to_string(std::numeric_limits<int64_t>::min()));
  under.Set("y", "1").Decr("m");
  EXPECT_EQ(db_.Write(under).code(), StatusCode::kOutOfRange);
  EXPECT_FALSE(db_.Exists("y"));
}

TEST_F(DbTest, BatchWithBadArgumentsIsRefusedBeforeAnythingChanges) {
  const std::string big_key(Db::kMaxKeySize + 1, 'k');
  for (const WriteBatch& bad : {
           WriteBatch().Set("ok", "1").Set(big_key, "v"),
           WriteBatch().Set("ok", "1").Rename("ok", big_key),
           WriteBatch().Set("ok", "1").Incr(big_key),
           WriteBatch().Set("ok", "1").Expire("ok", milliseconds::max()),
           WriteBatch().Set("ok", "1").Set("z", "2", Db::kMaxTtl * 2),
       }) {
    EXPECT_EQ(db_.Write(bad).code(), StatusCode::kInvalidArgument);
    EXPECT_FALSE(db_.Exists("ok"));
  }
}

TEST_F(DbTest, BatchAppendRespectsTheValueLimit) {
  std::string big(Db::kMaxValueSize, 'x');
  ASSERT_TRUE(db_.Set("k", big).ok());
  WriteBatch batch;
  batch.Set("z", "1").Append("k", "y");
  EXPECT_EQ(db_.Write(batch).code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("z"));
}

TEST_F(DbTest, MGetReadsValuesInOrderWithMissingAsNullopt) {
  Set("a", "1");
  Set("b", "2", milliseconds(10));
  Set("c", "");
  Advance(milliseconds(10));
  using Values = std::vector<std::optional<std::string>>;
  EXPECT_EQ(db_.MGet({"c", "a", "b", "nope", "a"}),
            (Values{"", "1", std::nullopt, std::nullopt, "1"}));
  EXPECT_EQ(db_.MGet({}), Values{});
}

TEST_F(DbTest, ConcurrentReadersNeverSeeAHalfAppliedBatch) {
  std::atomic<bool> done{false};
  std::atomic<int> torn_reads{0};
  std::atomic<int> reads{0};
  std::thread reader([&] {
    while (!done) {
      auto values = db_.MGet({"a", "b", "c"});
      if (values[0] != values[1] || values[1] != values[2]) ++torn_reads;
      ++reads;
    }
  });
  for (int i = 0; i < 3000; ++i) {
    const std::string v = std::to_string(i);
    WriteBatch batch;
    batch.Set("a", v).Set("b", v).Set("c", v);
    ASSERT_TRUE(db_.Write(batch).ok());
  }
  done = true;
  reader.join();
  EXPECT_EQ(torn_reads, 0);
  EXPECT_GT(reads, 0);
}

TEST_F(DbTest, IncrAndDecrNeverShortenAKeysTtl) {
  // Steps of half a millisecond: if each Incr re-derived the expiry from a
  // millisecond-rounded value, the TTL would shrink a little every call.
  Set("n", "0", seconds(10));
  Set("m", "0", seconds(10));
  for (int i = 0; i < 100; ++i) {
    now_ += std::chrono::microseconds(500);
    ASSERT_TRUE(db_.Incr("n").ok());
    ASSERT_TRUE(db_.Decr("m").ok());
    WriteBatch batch;
    batch.Incr("n").Decr("m");
    ASSERT_TRUE(db_.Write(batch).ok());
  }
  EXPECT_EQ(db_.Get("n"), "200");
  EXPECT_EQ(db_.Get("m"), "-200");
  // 100 steps of 0.5 ms have passed.
  EXPECT_EQ(db_.GetTtl("n").remaining, seconds(10) - milliseconds(50));
  EXPECT_EQ(db_.GetTtl("m").remaining, seconds(10) - milliseconds(50));
}

TEST_F(DbTest, DefaultClockConstructs) {
  Db db;
  ASSERT_TRUE(db.Set("k", "v", milliseconds(60000)).ok());
  EXPECT_EQ(db.Get("k"), "v");
}

}  // namespace
}  // namespace qey
