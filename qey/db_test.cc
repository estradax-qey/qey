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

using Pairs = std::vector<FieldValue>;

TEST_F(DbTest, HashSetCountsOnlyNewFields) {
  EXPECT_EQ(*db_.HSet("h", "a", "1"), 1);
  EXPECT_EQ(*db_.HSet("h", "a", "2"), 0);  // an existing field
  // "b" is new once, though it appears twice; "a" is not new; last value wins.
  EXPECT_EQ(*db_.HSet("h", {{"b", "2"}, {"a", "9"}, {"b", "3"}}), 1);
  EXPECT_EQ(**db_.HGet("h", "a"), "9");
  EXPECT_EQ(**db_.HGet("h", "b"), "3");
  EXPECT_EQ(*db_.HLen("h"), 2);
  EXPECT_EQ(*db_.HGetAll("h"), (Pairs{{"a", "9"}, {"b", "3"}}));
  EXPECT_EQ(*db_.HSet("h", Pairs{}), 0);  // nothing to set
  EXPECT_EQ(db_.Type("h"), KeyType::kHash);
}

TEST_F(DbTest, HashFieldsAreOrderedAndHoldAnyBytes) {
  ASSERT_TRUE(db_.HSet("h", {{"b", ""},
                             {"", "empty field"},
                             {std::string("\0x", 2), std::string("\0", 1)},
                             {"a", "va"}})
                  .ok());
  Pairs all = *db_.HGetAll("h");
  ASSERT_EQ(all.size(), 4);
  EXPECT_EQ(all[0], (FieldValue{"", "empty field"}));
  EXPECT_EQ(all[1], (FieldValue{std::string("\0x", 2), std::string("\0", 1)}));
  EXPECT_EQ(all[2], (FieldValue{"a", "va"}));
  EXPECT_EQ(all[3], (FieldValue{"b", ""}));
}

TEST_F(DbTest, MissingHashAndFieldReadAsEmpty) {
  EXPECT_FALSE((*db_.HGet("nope", "f")).has_value());
  EXPECT_FALSE(*db_.HExists("nope", "f"));
  EXPECT_EQ(*db_.HLen("nope"), 0);
  EXPECT_TRUE(db_.HGetAll("nope")->empty());
  EXPECT_EQ(*db_.HDel("nope", {"f"}), 0);
  ASSERT_TRUE(db_.HSet("h", "a", "1").ok());
  EXPECT_FALSE((*db_.HGet("h", "zzz")).has_value());
  EXPECT_FALSE(*db_.HExists("h", "zzz"));
  EXPECT_TRUE(*db_.HExists("h", "a"));
  EXPECT_EQ(db_.Type("nope"), KeyType::kNone);
}

TEST_F(DbTest, HDelCountsDistinctPresentFieldsAndRemovesAnEmptiedKey) {
  ASSERT_TRUE(db_.HSet("h", {{"a", "1"}, {"b", "2"}, {"c", "3"}}).ok());
  EXPECT_EQ(*db_.HDel("h", {"a", "a", "nope"}), 1);
  EXPECT_EQ(*db_.HLen("h"), 2);
  EXPECT_EQ(*db_.HDel("h", {"nope"}), 0);
  EXPECT_EQ(*db_.HDel("h", std::vector<std::string>{}), 0);
  ASSERT_TRUE(db_.Keys().size() == 1);

  EXPECT_EQ(*db_.HDel("h", {"b", "c"}), 2);
  EXPECT_EQ(db_.Type("h"), KeyType::kNone);  // the last field took the key
  EXPECT_FALSE(db_.Exists("h"));
  EXPECT_EQ(db_.Keys(), Keys{});
  EXPECT_EQ(db_.Size(), 0);
}

TEST_F(DbTest, HIncrCreatesAddsAndRejectsBadValues) {
  EXPECT_EQ(*db_.HIncr("h", "n", 5), 5);  // creates the hash and the field
  EXPECT_EQ(*db_.HIncr("h", "n"), 6);
  EXPECT_EQ(*db_.HIncr("h", "m", -2), -2);  // new field in an existing hash
  EXPECT_EQ(**db_.HGet("h", "n"), "6");

  ASSERT_TRUE(db_.HSet("h", "text", "abc").ok());
  EXPECT_EQ(db_.HIncr("h", "text").status().code(), StatusCode::kNotInteger);
  EXPECT_EQ(**db_.HGet("h", "text"), "abc");

  ASSERT_TRUE(
      db_.HSet("h", "big", std::to_string(std::numeric_limits<int64_t>::max()))
          .ok());
  EXPECT_EQ(db_.HIncr("h", "big").status().code(), StatusCode::kOutOfRange);
}

TEST_F(DbTest, HIncrOnAMissingKeyThatFailsLeavesNoKey) {
  EXPECT_EQ(*db_.HIncr("fresh", "f", std::numeric_limits<int64_t>::max()),
            std::numeric_limits<int64_t>::max());
  EXPECT_EQ(db_.HIncr("fresh", "f", 1).status().code(),
            StatusCode::kOutOfRange);
  EXPECT_EQ(*db_.HLen("fresh"), 1);
}

TEST_F(DbTest, EveryOperationChecksTheTypeOfTheKey) {
  Set("s", "string");
  ASSERT_TRUE(db_.HSet("h", "f", "v").ok());
  const StatusCode wrong = StatusCode::kWrongType;

  // Hash commands on a string.
  EXPECT_EQ(db_.HSet("s", "f", "v").status().code(), wrong);
  EXPECT_EQ(db_.HGet("s", "f").status().code(), wrong);
  EXPECT_EQ(db_.HDel("s", {"f"}).status().code(), wrong);
  EXPECT_EQ(db_.HExists("s", "f").status().code(), wrong);
  EXPECT_EQ(db_.HLen("s").status().code(), wrong);
  EXPECT_EQ(db_.HGetAll("s").status().code(), wrong);
  EXPECT_EQ(db_.HIncr("s", "f").status().code(), wrong);
  EXPECT_EQ(db_.Get("s"), "string");  // untouched

  // String commands on a hash.
  EXPECT_EQ(db_.Append("h", "x").status().code(), wrong);
  EXPECT_EQ(db_.Incr("h").status().code(), wrong);
  EXPECT_EQ(db_.Decr("h").status().code(), wrong);
  EXPECT_EQ(db_.Get("h"), std::nullopt);
  EXPECT_EQ(db_.MGet({"s", "h"}),
            (std::vector<std::optional<std::string>>{"string", std::nullopt}));
  EXPECT_EQ(*db_.HGetAll("h"), (Pairs{{"f", "v"}}));  // untouched

  // Commands that work on any type.
  EXPECT_TRUE(db_.Exists("h"));
  EXPECT_EQ(db_.Type("h"), KeyType::kHash);
  EXPECT_EQ(db_.Type("s"), KeyType::kString);
  EXPECT_TRUE(*db_.Expire("h", milliseconds(100)));
  EXPECT_EQ(db_.GetTtl("h").remaining, milliseconds(100));
  EXPECT_TRUE(*db_.Persist("h"));
  EXPECT_TRUE(*db_.Rename("h", "h2"));
  EXPECT_EQ(db_.Type("h2"), KeyType::kHash);
  EXPECT_TRUE(*db_.Del("h2"));
}

TEST_F(DbTest, SettingAStringReplacesAHashAndItsTtl) {
  ASSERT_TRUE(db_.HSet("k", {{"a", "1"}, {"b", "2"}}).ok());
  ASSERT_TRUE(*db_.Expire("k", milliseconds(100)));
  Set("k", "now a string");
  EXPECT_EQ(db_.Type("k"), KeyType::kString);
  EXPECT_EQ(db_.Get("k"), "now a string");
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, AHashExpiresAsAWholeAndAnExpiredOneStartsOverEmpty) {
  ASSERT_TRUE(db_.HSet("h", {{"a", "1"}, {"b", "2"}}).ok());
  ASSERT_TRUE(*db_.Expire("h", milliseconds(100)));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Type("h"), KeyType::kNone);
  EXPECT_TRUE(db_.HGetAll("h")->empty());

  // Writing again must not bring back the old fields or the old TTL.
  EXPECT_EQ(*db_.HSet("h", "c", "3"), 1);
  EXPECT_EQ(*db_.HGetAll("h"), (Pairs{{"c", "3"}}));
  EXPECT_EQ(db_.GetTtl("h").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, RenamingAHashKeepsItsFieldsAndTtl) {
  ASSERT_TRUE(db_.HSet("a", {{"x", "1"}, {"y", "2"}}).ok());
  ASSERT_TRUE(*db_.Expire("a", milliseconds(100)));
  Set("b", "old");
  ASSERT_TRUE(*db_.Rename("a", "b"));  // replaces a string with a hash
  EXPECT_EQ(db_.Type("b"), KeyType::kHash);
  EXPECT_EQ(*db_.HGetAll("b"), (Pairs{{"x", "1"}, {"y", "2"}}));
  EXPECT_EQ(db_.GetTtl("b").remaining, milliseconds(100));
  EXPECT_EQ(db_.Keys(), Keys{"b"});
}

TEST_F(DbTest, OversizedHashElementsAreRejected) {
  const std::string big_key(Db::kMaxKeySize + 1, 'k');
  EXPECT_EQ(db_.HSet(big_key, "f", "v").status().code(),
            StatusCode::kInvalidArgument);
  std::string big(Db::kMaxValueSize + 1, 'x');
  EXPECT_EQ(db_.HSet("h", "f", big).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.HSet("h", big, "v").status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.HIncr("h", big).status().code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("h"));
}

TEST_F(DbTest, HashesInABatch) {
  Set("s", "text");
  WriteBatch batch;
  batch.HSet("h", "a", "1")
      .HSet("h", {{"b", "2"}, {"c", "3"}})
      .HDel("h", {"b"})
      .HIncr("h", "n", 7)
      .HIncr("h", "n")
      .Set("t", "v");
  ASSERT_TRUE(db_.Write(batch).ok());
  EXPECT_EQ(*db_.HGetAll("h"), (Pairs{{"a", "1"}, {"c", "3"}, {"n", "8"}}));
  EXPECT_EQ(db_.Keys(), (Keys{"h", "s", "t"}));
}

TEST_F(DbTest, AFailedBatchPutsAHashBackAsItWas) {
  ASSERT_TRUE(db_.HSet("h", {{"a", "1"}, {"b", "x"}}).ok());
  ASSERT_TRUE(*db_.Expire("h", milliseconds(100)));
  Set("s", "text");
  ASSERT_EQ(db_.Keys().size(), 2);  // builds the key index

  WriteBatch batch;
  batch.HSet("h", "new", "field")
      .HDel("h", {"a"})
      .HSet("fresh", "f", "v")  // a hash that only exists inside the batch
      .Rename("h", "moved")
      .HIncr("moved", "b");  // "x" is not an integer: the batch fails
  EXPECT_EQ(db_.Write(batch).code(), StatusCode::kNotInteger);

  EXPECT_EQ(*db_.HGetAll("h"), (Pairs{{"a", "1"}, {"b", "x"}}));
  EXPECT_EQ(db_.GetTtl("h").remaining, milliseconds(100));
  EXPECT_EQ(db_.Type("fresh"), KeyType::kNone);
  EXPECT_EQ(db_.Type("moved"), KeyType::kNone);
  EXPECT_EQ(db_.Keys(), (Keys{"h", "s"}));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Sweep(), 1);
  EXPECT_EQ(db_.Keys(), Keys{"s"});
}

TEST_F(DbTest, HashKeysAreListedAmongTheKeys) {
  ASSERT_TRUE(db_.HSet("user:1", "name", "ann").ok());
  ASSERT_TRUE(db_.HSet("user:2", "name", "bo").ok());
  Set("other", "v");
  EXPECT_EQ(db_.Keys("user:"), (Keys{"user:1", "user:2"}));
  EXPECT_EQ(db_.Keys(), (Keys{"other", "user:1", "user:2"}));
}

using Strings = std::vector<std::string>;

TEST_F(DbTest, ListPushesGoToTheHeadOneAtATimeOrTheTailInOrder) {
  EXPECT_EQ(*db_.RPush("l", {"a", "b", "c"}), 3);
  EXPECT_EQ(*db_.LPush("l", {"x", "y"}), 5);  // y ends up first
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"y", "x", "a", "b", "c"}));
  EXPECT_EQ(*db_.LLen("l"), 5);
  EXPECT_EQ(db_.Type("l"), KeyType::kList);
  EXPECT_EQ(*db_.RPush("l", Strings{}), 5);  // nothing to push
  EXPECT_EQ(*db_.LPush("fresh", Strings{}), 0);
  EXPECT_FALSE(db_.Exists("fresh"));
}

TEST_F(DbTest, ListElementsMayBeEmptyAndHoldAnyBytes) {
  ASSERT_TRUE(db_.RPush("l", {"", std::string("\0x", 2), "z"}).ok());
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"", std::string("\0x", 2), "z"}));
  EXPECT_EQ(**db_.LIndex("l", 0), "");
}

TEST_F(DbTest, ListIndexingCountsFromEitherEnd) {
  ASSERT_TRUE(db_.RPush("l", {"a", "b", "c"}).ok());
  EXPECT_EQ(**db_.LIndex("l", 0), "a");
  EXPECT_EQ(**db_.LIndex("l", 2), "c");
  EXPECT_EQ(**db_.LIndex("l", -1), "c");
  EXPECT_EQ(**db_.LIndex("l", -3), "a");
  EXPECT_FALSE((*db_.LIndex("l", 3)).has_value());
  EXPECT_FALSE((*db_.LIndex("l", -4)).has_value());
  EXPECT_FALSE(
      (*db_.LIndex("l", std::numeric_limits<int64_t>::min())).has_value());
  EXPECT_FALSE(
      (*db_.LIndex("l", std::numeric_limits<int64_t>::max())).has_value());
  EXPECT_FALSE((*db_.LIndex("nope", 0)).has_value());
}

TEST_F(DbTest, ListRangesAreInclusiveAndClamped) {
  ASSERT_TRUE(db_.RPush("l", {"a", "b", "c", "d", "e"}).ok());
  EXPECT_EQ(*db_.LRange("l", 1, 3), (Strings{"b", "c", "d"}));
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"a", "b", "c", "d", "e"}));
  EXPECT_EQ(*db_.LRange("l", -2, -1), (Strings{"d", "e"}));
  EXPECT_EQ(*db_.LRange("l", -100, 100), (Strings{"a", "b", "c", "d", "e"}));
  EXPECT_EQ(*db_.LRange("l", 3, 1), Strings{});
  EXPECT_EQ(*db_.LRange("l", 5, 9), Strings{});
  EXPECT_EQ(*db_.LRange("l", -100, -50), Strings{});
  EXPECT_EQ(*db_.LRange("l", 2, 2), Strings{"c"});
  EXPECT_EQ(*db_.LRange("l", std::numeric_limits<int64_t>::min(),
                        std::numeric_limits<int64_t>::max()),
            (Strings{"a", "b", "c", "d", "e"}));
  EXPECT_EQ(*db_.LRange("nope", 0, -1), Strings{});
}

TEST_F(DbTest, PoppingReturnsElementsInTheOrderTheyLeave) {
  ASSERT_TRUE(db_.RPush("l", {"a", "b", "c", "d", "e"}).ok());
  EXPECT_EQ(*db_.LPop("l"), Strings{"a"});
  EXPECT_EQ(*db_.RPop("l"), Strings{"e"});
  EXPECT_EQ(*db_.LPop("l", 2), (Strings{"b", "c"}));
  EXPECT_EQ(*db_.LRange("l", 0, -1), Strings{"d"});
  EXPECT_EQ(*db_.LPop("l", 0), Strings{});
  EXPECT_EQ(*db_.RPop("nope"), Strings{});
  EXPECT_EQ(*db_.LLen("l"), 1);
}

TEST_F(DbTest, PoppingMoreThanThereIsEmptiesTheListAndRemovesTheKey) {
  ASSERT_TRUE(db_.RPush("l", {"a", "b", "c"}).ok());
  EXPECT_EQ(*db_.RPop("l", 100), (Strings{"c", "b", "a"}));
  EXPECT_EQ(db_.Type("l"), KeyType::kNone);
  EXPECT_FALSE(db_.Exists("l"));
  EXPECT_EQ(db_.Keys(), Keys{});
  EXPECT_EQ(*db_.LPop("l", std::numeric_limits<size_t>::max()), Strings{});
}

TEST_F(DbTest, LSetReplacesAnElementOrReportsFalse) {
  ASSERT_TRUE(db_.RPush("l", {"a", "b", "c"}).ok());
  EXPECT_TRUE(*db_.LSet("l", 1, "B"));
  EXPECT_TRUE(*db_.LSet("l", -1, "C"));
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"a", "B", "C"}));
  EXPECT_FALSE(*db_.LSet("l", 3, "x"));
  EXPECT_FALSE(*db_.LSet("l", -4, "x"));
  EXPECT_FALSE(*db_.LSet("nope", 0, "x"));
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"a", "B", "C"}));
}

TEST_F(DbTest, LTrimKeepsARange) {
  auto fill = [&] {
    ASSERT_TRUE(db_.Del("l").ok());
    ASSERT_TRUE(db_.RPush("l", {"a", "b", "c", "d", "e"}).ok());
  };
  fill();
  ASSERT_TRUE(db_.LTrim("l", 1, 3).ok());
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"b", "c", "d"}));
  fill();
  ASSERT_TRUE(db_.LTrim("l", -2, -1).ok());
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"d", "e"}));
  fill();
  ASSERT_TRUE(db_.LTrim("l", 0, -1).ok());  // keeps everything
  EXPECT_EQ(*db_.LLen("l"), 5);
  ASSERT_TRUE(db_.LTrim("l", -100, 100).ok());
  EXPECT_EQ(*db_.LLen("l"), 5);
  ASSERT_TRUE(db_.LTrim("l", 2, 2).ok());
  EXPECT_EQ(*db_.LRange("l", 0, -1), Strings{"c"});
  ASSERT_TRUE(db_.LTrim("nope", 0, 1).ok());  // not an error
}

TEST_F(DbTest, LTrimKeepingNothingRemovesTheKey) {
  for (auto [start, stop] :
       {std::pair<int64_t, int64_t>{3, 1}, {5, 9}, {-100, -50}}) {
    ASSERT_TRUE(db_.RPush("l", {"a", "b", "c"}).ok());
    ASSERT_TRUE(db_.LTrim("l", start, stop).ok());
    EXPECT_EQ(db_.Type("l"), KeyType::kNone) << start << " " << stop;
  }
}

TEST_F(DbTest, ListCommandsCheckTheTypeOfTheKey) {
  Set("s", "string");
  ASSERT_TRUE(db_.RPush("l", {"a"}).ok());
  const StatusCode wrong = StatusCode::kWrongType;
  EXPECT_EQ(db_.LPush("s", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.RPush("s", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.LPop("s").status().code(), wrong);
  EXPECT_EQ(db_.RPop("s").status().code(), wrong);
  EXPECT_EQ(db_.LLen("s").status().code(), wrong);
  EXPECT_EQ(db_.LIndex("s", 0).status().code(), wrong);
  EXPECT_EQ(db_.LRange("s", 0, -1).status().code(), wrong);
  EXPECT_EQ(db_.LSet("s", 0, "x").status().code(), wrong);
  EXPECT_EQ(db_.LTrim("s", 0, 1).code(), wrong);
  EXPECT_EQ(db_.Get("s"), "string");

  ASSERT_TRUE(db_.HSet("h", "f", "v").ok());
  EXPECT_EQ(db_.LPush("h", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.HSet("l", "f", "v").status().code(), wrong);
  EXPECT_EQ(db_.Append("l", "x").status().code(), wrong);
  EXPECT_EQ(db_.Incr("l").status().code(), wrong);
  EXPECT_EQ(db_.Get("l"), std::nullopt);
  EXPECT_EQ(*db_.LRange("l", 0, -1), Strings{"a"});  // untouched
}

TEST_F(DbTest, ListsExpireRenameAndAreReplacedLikeAnyKey) {
  ASSERT_TRUE(db_.RPush("a", {"x", "y"}).ok());
  ASSERT_TRUE(*db_.Expire("a", milliseconds(100)));
  ASSERT_TRUE(*db_.Rename("a", "b"));
  EXPECT_EQ(*db_.LRange("b", 0, -1), (Strings{"x", "y"}));
  EXPECT_EQ(db_.GetTtl("b").remaining, milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Type("b"), KeyType::kNone);
  EXPECT_EQ(*db_.RPush("b", {"fresh"}), 1);  // not "x", "y", "fresh"
  EXPECT_EQ(db_.GetTtl("b").state, Db::TtlState::kNoExpiry);

  Set("b", "now a string");
  EXPECT_EQ(db_.Type("b"), KeyType::kString);
}

TEST_F(DbTest, OversizedListElementsAreRejected) {
  std::string big(Db::kMaxValueSize + 1, 'x');
  EXPECT_EQ(db_.RPush("l", {"ok", big}).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.LSet("l", 0, big).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("l"));
}

TEST_F(DbTest, ListsInABatch) {
  WriteBatch batch;
  batch.RPush("l", {"a", "b", "c", "d"})
      .LPush("l", {"z"})
      .LPop("l")
      .RPop("l", 2)
      .LSet("l", 0, "A")
      .RPush("q", {"1", "2", "3"})
      .LTrim("q", 1, -1);
  ASSERT_TRUE(db_.Write(batch).ok());
  // [a b c d] -> [z a b c d] -> [a b c d] -> [a b] -> [A b]
  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"A", "b"}));
  EXPECT_EQ(*db_.LRange("q", 0, -1), (Strings{"2", "3"}));
}

TEST_F(DbTest, AFailedBatchPutsAListBackAsItWas) {
  ASSERT_TRUE(db_.RPush("l", {"a", "b", "c"}).ok());
  ASSERT_TRUE(*db_.Expire("l", milliseconds(100)));
  Set("text", "abc");
  ASSERT_EQ(db_.Keys().size(), 2);  // builds the key index

  WriteBatch batch;
  batch.LPush("l", {"x"})
      .RPop("l", 2)
      .LTrim("l", 0, 0)
      .RPush("fresh", {"n"})
      .LPop("fresh")  // empties and removes a list created in this batch
      .Incr("text");  // fails
  EXPECT_EQ(db_.Write(batch).code(), StatusCode::kNotInteger);

  EXPECT_EQ(*db_.LRange("l", 0, -1), (Strings{"a", "b", "c"}));
  EXPECT_EQ(db_.GetTtl("l").remaining, milliseconds(100));
  EXPECT_EQ(db_.Type("fresh"), KeyType::kNone);
  EXPECT_EQ(db_.Keys(), (Keys{"l", "text"}));
}

TEST_F(DbTest, SetAddCountsOnlyNewMembers) {
  EXPECT_EQ(*db_.SAdd("s", {"b", "a", "b"}), 2);  // "b" twice counts once
  EXPECT_EQ(*db_.SAdd("s", {"a", "c"}), 1);
  EXPECT_EQ(*db_.SAdd("s", {"a"}), 0);
  EXPECT_EQ(*db_.SAdd("s", Strings{}), 0);
  EXPECT_EQ(*db_.SMembers("s"), (Strings{"a", "b", "c"}));  // byte order
  EXPECT_EQ(*db_.SCard("s"), 3);
  EXPECT_EQ(db_.Type("s"), KeyType::kSet);
  EXPECT_EQ(*db_.SAdd("fresh", Strings{}), 0);
  EXPECT_FALSE(db_.Exists("fresh"));
}

TEST_F(DbTest, SetMembersMayBeEmptyAndHoldAnyBytes) {
  ASSERT_TRUE(db_.SAdd("s", {"", std::string("\0x", 2), "z"}).ok());
  EXPECT_TRUE(*db_.SIsMember("s", ""));
  EXPECT_TRUE(*db_.SIsMember("s", std::string("\0x", 2)));
  EXPECT_FALSE(*db_.SIsMember("s", std::string("\0", 1)));
  EXPECT_EQ(db_.SMembers("s")->front(), "");
}

TEST_F(DbTest, MissingSetsReadAsEmpty) {
  EXPECT_FALSE(*db_.SIsMember("nope", "a"));
  EXPECT_EQ(*db_.SCard("nope"), 0);
  EXPECT_EQ(*db_.SMembers("nope"), Strings{});
  EXPECT_EQ(*db_.SRem("nope", {"a"}), 0);
}

TEST_F(DbTest, SetRemoveCountsDistinctPresentMembersAndRemovesAnEmptiedKey) {
  ASSERT_TRUE(db_.SAdd("s", {"a", "b", "c"}).ok());
  EXPECT_EQ(*db_.SRem("s", {"a", "a", "nope"}), 1);
  EXPECT_EQ(*db_.SRem("s", {"nope"}), 0);
  EXPECT_EQ(*db_.SRem("s", Strings{}), 0);
  EXPECT_EQ(*db_.SMembers("s"), (Strings{"b", "c"}));
  EXPECT_EQ(*db_.SRem("s", {"b", "c"}), 2);
  EXPECT_EQ(db_.Type("s"), KeyType::kNone);
  EXPECT_EQ(db_.Keys(), Keys{});
}

TEST_F(DbTest, SetAlgebraOverSeveralSets) {
  ASSERT_TRUE(db_.SAdd("a", {"1", "2", "3", "4"}).ok());
  ASSERT_TRUE(db_.SAdd("b", {"3", "4", "5"}).ok());
  ASSERT_TRUE(db_.SAdd("c", {"4", "5", "6"}).ok());
  EXPECT_EQ(*db_.SUnion({"a", "b", "c"}),
            (Strings{"1", "2", "3", "4", "5", "6"}));
  EXPECT_EQ(*db_.SInter({"a", "b", "c"}), Strings{"4"});
  EXPECT_EQ(*db_.SInter({"a", "b"}), (Strings{"3", "4"}));
  EXPECT_EQ(*db_.SDiff({"a", "b", "c"}), (Strings{"1", "2"}));
  EXPECT_EQ(*db_.SDiff({"b", "a"}), Strings{"5"});
  EXPECT_EQ(*db_.SUnion({"a"}), (Strings{"1", "2", "3", "4"}));
  EXPECT_EQ(*db_.SInter({"a", "a"}), (Strings{"1", "2", "3", "4"}));
}

TEST_F(DbTest, SetAlgebraTreatsMissingKeysAsEmptyAndNoKeysAsNothing) {
  ASSERT_TRUE(db_.SAdd("a", {"1", "2"}).ok());
  EXPECT_EQ(*db_.SUnion({"a", "nope"}), (Strings{"1", "2"}));
  EXPECT_EQ(*db_.SInter({"a", "nope"}), Strings{});
  EXPECT_EQ(*db_.SDiff({"a", "nope"}), (Strings{"1", "2"}));
  EXPECT_EQ(*db_.SDiff({"nope", "a"}), Strings{});
  EXPECT_EQ(*db_.SUnion(Strings{}), Strings{});
  EXPECT_EQ(*db_.SInter(Strings{}), Strings{});
  EXPECT_EQ(*db_.SDiff(Strings{}), Strings{});
}

TEST_F(DbTest, SetAlgebraFailsIfAnyKeyIsNotASetWhateverTheOthersHold) {
  ASSERT_TRUE(db_.SAdd("a", {"1"}).ok());
  Set("s", "string");
  ASSERT_TRUE(db_.RPush("l", {"x"}).ok());
  EXPECT_EQ(db_.SUnion({"a", "s"}).status().code(), StatusCode::kWrongType);
  EXPECT_EQ(db_.SInter({"nope", "s"}).status().code(), StatusCode::kWrongType);
  EXPECT_EQ(db_.SDiff({"a", "nope", "l"}).status().code(),
            StatusCode::kWrongType);
}

TEST_F(DbTest, SetCommandsCheckTheTypeOfTheKey) {
  Set("s", "string");
  ASSERT_TRUE(db_.SAdd("set", {"a"}).ok());
  const StatusCode wrong = StatusCode::kWrongType;
  EXPECT_EQ(db_.SAdd("s", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.SRem("s", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.SIsMember("s", "a").status().code(), wrong);
  EXPECT_EQ(db_.SCard("s").status().code(), wrong);
  EXPECT_EQ(db_.SMembers("s").status().code(), wrong);
  EXPECT_EQ(db_.Get("s"), "string");

  EXPECT_EQ(db_.HSet("set", "f", "v").status().code(), wrong);
  EXPECT_EQ(db_.RPush("set", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.Append("set", "x").status().code(), wrong);
  EXPECT_EQ(db_.Get("set"), std::nullopt);
  EXPECT_EQ(*db_.SMembers("set"), Strings{"a"});  // untouched
}

TEST_F(DbTest, SetsExpireRenameAndAreReplacedLikeAnyKey) {
  ASSERT_TRUE(db_.SAdd("a", {"x", "y"}).ok());
  ASSERT_TRUE(*db_.Expire("a", milliseconds(100)));
  ASSERT_TRUE(*db_.Rename("a", "b"));
  EXPECT_EQ(*db_.SMembers("b"), (Strings{"x", "y"}));
  EXPECT_EQ(db_.GetTtl("b").remaining, milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Type("b"), KeyType::kNone);
  EXPECT_EQ(*db_.SAdd("b", {"fresh"}), 1);
  EXPECT_EQ(*db_.SMembers("b"), Strings{"fresh"});
  EXPECT_EQ(db_.GetTtl("b").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, OversizedSetMembersAreRejected) {
  std::string big(Db::kMaxValueSize + 1, 'x');
  EXPECT_EQ(db_.SAdd("s", {"ok", big}).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("s"));
}

TEST_F(DbTest, SetsInABatchAndARolledBackBatch) {
  ASSERT_TRUE(db_.SAdd("s", {"a", "b", "c"}).ok());
  ASSERT_TRUE(*db_.Expire("s", milliseconds(100)));
  Set("text", "abc");
  ASSERT_EQ(db_.Keys().size(), 2);

  WriteBatch ok_batch;
  ok_batch.SAdd("s", {"d"})
      .SRem("s", {"a"})
      .SAdd("t", {"x", "y"})
      .SRem("t", {"x"});
  ASSERT_TRUE(db_.Write(ok_batch).ok());
  EXPECT_EQ(*db_.SMembers("s"), (Strings{"b", "c", "d"}));
  EXPECT_EQ(*db_.SMembers("t"), Strings{"y"});

  WriteBatch bad_batch;
  bad_batch.SAdd("s", {"zzz"})
      .SRem("s", {"b", "c"})
      .SAdd("u", {"n"})
      .Incr("text");
  EXPECT_EQ(db_.Write(bad_batch).code(), StatusCode::kNotInteger);
  EXPECT_EQ(*db_.SMembers("s"), (Strings{"b", "c", "d"}));
  EXPECT_EQ(db_.GetTtl("s").remaining, milliseconds(100));
  EXPECT_EQ(db_.Type("u"), KeyType::kNone);
  EXPECT_EQ(db_.Keys(), (Keys{"s", "t", "text"}));
}

using Scored = std::vector<ScoredMember>;
constexpr double kInf = std::numeric_limits<double>::infinity();

TEST_F(DbTest, SortedSetAddCountsNewMembersAndUpdatesScores) {
  EXPECT_EQ(*db_.ZAdd("z", {{"a", 1}, {"b", 2}, {"c", 3}}), 3);
  EXPECT_EQ(*db_.ZAdd("z", {{"b", 20}, {"d", 4}}), 1);  // "b" only moves
  // A repeated member counts once, with the last score.
  EXPECT_EQ(*db_.ZAdd("z", {{"e", 1}, {"e", 5}}), 1);
  EXPECT_EQ(**db_.ZScore("z", "b"), 20);
  EXPECT_EQ(**db_.ZScore("z", "e"), 5);
  EXPECT_EQ(*db_.ZCard("z"), 5);
  EXPECT_EQ(*db_.ZAdd("z", {{"a", 1}}), 0);  // already so
  EXPECT_EQ(*db_.ZAdd("z", Scored{}), 0);
  EXPECT_EQ(db_.Type("z"), KeyType::kZSet);
  EXPECT_FALSE((*db_.ZScore("z", "nope")).has_value());
}

TEST_F(DbTest, SortedSetOrdersByScoreThenMemberBytes) {
  ASSERT_TRUE(db_.ZAdd("z", {{"b", 1},
                             {"a", 1},
                             {"c", -5},
                             {"inf", kInf},
                             {"-inf", -kInf},
                             {"", 0},
                             {"a2", 1}})
                  .ok());
  EXPECT_EQ(*db_.ZRange("z", 0, -1), (Scored{{"-inf", -kInf},
                                             {"c", -5},
                                             {"", 0},
                                             {"a", 1},
                                             {"a2", 1},
                                             {"b", 1},
                                             {"inf", kInf}}));
}

TEST_F(DbTest, SortedSetScoresKeepTheirExactValue) {
  const std::vector<double> scores = {0.1, 1.0 / 3.0, 1e300, 5e-324, -0.0};
  Scored members;
  for (size_t i = 0; i < scores.size(); ++i) {
    members.push_back({"m" + std::to_string(i), scores[i]});
  }
  ASSERT_TRUE(db_.ZAdd("z", members).ok());
  for (size_t i = 0; i < scores.size(); ++i) {
    EXPECT_EQ(**db_.ZScore("z", "m" + std::to_string(i)), scores[i]);
  }
}

TEST_F(DbTest, SortedSetRankCountsFromTheLowestScore) {
  ASSERT_TRUE(db_.ZAdd("z", {{"a", 3}, {"b", 1}, {"c", 2}}).ok());
  EXPECT_EQ(**db_.ZRank("z", "b"), 0);
  EXPECT_EQ(**db_.ZRank("z", "c"), 1);
  EXPECT_EQ(**db_.ZRank("z", "a"), 2);
  EXPECT_FALSE((*db_.ZRank("z", "nope")).has_value());
  EXPECT_FALSE((*db_.ZRank("nope", "a")).has_value());
  ASSERT_TRUE(db_.ZAdd("z", {{"a", 0}}).ok());  // moves to the front
  EXPECT_EQ(**db_.ZRank("z", "a"), 0);
  EXPECT_EQ(**db_.ZRank("z", "b"), 1);
}

TEST_F(DbTest, SortedSetRangesByPosition) {
  ASSERT_TRUE(
      db_.ZAdd("z", {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}, {"e", 5}}).ok());
  EXPECT_EQ(*db_.ZRange("z", 1, 2), (Scored{{"b", 2}, {"c", 3}}));
  EXPECT_EQ(*db_.ZRange("z", -2, -1), (Scored{{"d", 4}, {"e", 5}}));
  EXPECT_EQ(db_.ZRange("z", 0, -1)->size(), 5);
  EXPECT_EQ(*db_.ZRange("z", -100, 100), *db_.ZRange("z", 0, -1));
  EXPECT_EQ(*db_.ZRange("z", 3, 1), Scored{});
  EXPECT_EQ(*db_.ZRange("z", 9, 12), Scored{});
  EXPECT_EQ(*db_.ZRange("nope", 0, -1), Scored{});
}

TEST_F(DbTest, SortedSetRangesByScoreIncludeBothBounds) {
  ASSERT_TRUE(
      db_.ZAdd("z", {{"a", 1}, {"b", 2}, {"c", 2}, {"d", 3}, {"e", kInf}})
          .ok());
  EXPECT_EQ(*db_.ZRangeByScore("z", 2, 3),
            (Scored{{"b", 2}, {"c", 2}, {"d", 3}}));
  EXPECT_EQ(*db_.ZRangeByScore("z", 2, 2), (Scored{{"b", 2}, {"c", 2}}));
  EXPECT_EQ(*db_.ZRangeByScore("z", -kInf, 1), (Scored{{"a", 1}}));
  EXPECT_EQ(db_.ZRangeByScore("z", 3, kInf)->size(), 2);
  EXPECT_EQ(*db_.ZRangeByScore("z", 5, 9), Scored{});
  EXPECT_EQ(*db_.ZRangeByScore("z", 3, 2), Scored{});  // min above max
  EXPECT_EQ(*db_.ZRangeByScore("nope", 0, 1), Scored{});
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(db_.ZRangeByScore("z", nan, 1).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.ZRangeByScore("z", 1, nan).status().code(),
            StatusCode::kInvalidArgument);
}

TEST_F(DbTest, SortedSetRemoveCountsPresentMembersAndRemovesAnEmptiedKey) {
  ASSERT_TRUE(db_.ZAdd("z", {{"a", 1}, {"b", 2}, {"c", 3}}).ok());
  EXPECT_EQ(*db_.ZRem("z", {"a", "a", "nope"}), 1);
  EXPECT_EQ(*db_.ZRem("z", {"nope"}), 0);
  EXPECT_EQ(*db_.ZRem("z", Strings{}), 0);
  EXPECT_EQ(*db_.ZCard("z"), 2);
  EXPECT_EQ(*db_.ZRem("z", {"b", "c"}), 2);
  EXPECT_EQ(db_.Type("z"), KeyType::kNone);
  EXPECT_EQ(db_.Keys(), Keys{});
  EXPECT_EQ(*db_.ZRem("nope", {"a"}), 0);
}

TEST_F(DbTest, ZIncrCreatesAddsAndRefusesAnInvalidResult) {
  EXPECT_EQ(*db_.ZIncr("z", "a", 2.5), 2.5);  // creates the set and member
  EXPECT_EQ(*db_.ZIncr("z", "a"), 3.5);
  EXPECT_EQ(*db_.ZIncr("z", "b", -1), -1);
  EXPECT_EQ(**db_.ZScore("z", "a"), 3.5);
  EXPECT_EQ(*db_.ZIncr("z", "a", 0), 3.5);  // no change

  ASSERT_TRUE(db_.ZAdd("z", {{"up", kInf}}).ok());
  EXPECT_EQ(db_.ZIncr("z", "up", -kInf).status().code(),
            StatusCode::kInvalidArgument);  // inf + -inf is not a number
  EXPECT_EQ(**db_.ZScore("z", "up"), kInf);
  EXPECT_EQ(db_.ZIncr("z", "a", std::numeric_limits<double>::quiet_NaN())
                .status()
                .code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(*db_.ZIncr("fresh", "m", 0), 0);  // a zero delta still creates
  EXPECT_EQ(db_.Type("fresh"), KeyType::kZSet);
}

TEST_F(DbTest, SortedSetsRefuseNaNScores) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(db_.ZAdd("z", {{"ok", 1}, {"bad", nan}}).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_FALSE(db_.Exists("z"));
}

TEST_F(DbTest, SortedSetCommandsCheckTheTypeOfTheKey) {
  Set("s", "string");
  ASSERT_TRUE(db_.ZAdd("z", {{"a", 1}}).ok());
  const StatusCode wrong = StatusCode::kWrongType;
  EXPECT_EQ(db_.ZAdd("s", {{"a", 1}}).status().code(), wrong);
  EXPECT_EQ(db_.ZRem("s", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.ZScore("s", "a").status().code(), wrong);
  EXPECT_EQ(db_.ZRank("s", "a").status().code(), wrong);
  EXPECT_EQ(db_.ZCard("s").status().code(), wrong);
  EXPECT_EQ(db_.ZRange("s", 0, -1).status().code(), wrong);
  EXPECT_EQ(db_.ZRangeByScore("s", 0, 1).status().code(), wrong);
  EXPECT_EQ(db_.ZIncr("s", "a").status().code(), wrong);

  ASSERT_TRUE(db_.SAdd("set", {"a"}).ok());
  EXPECT_EQ(db_.ZAdd("set", {{"a", 1}}).status().code(), wrong);
  EXPECT_EQ(db_.SAdd("z", {"a"}).status().code(), wrong);
  EXPECT_EQ(db_.Incr("z").status().code(), wrong);
  EXPECT_EQ(db_.Get("z"), std::nullopt);
  EXPECT_EQ(*db_.ZRange("z", 0, -1), (Scored{{"a", 1}}));  // untouched
}

TEST_F(DbTest, SortedSetsExpireRenameAndAreReplacedLikeAnyKey) {
  ASSERT_TRUE(db_.ZAdd("a", {{"x", 1}, {"y", 2}}).ok());
  ASSERT_TRUE(*db_.Expire("a", milliseconds(100)));
  ASSERT_TRUE(*db_.Rename("a", "b"));
  EXPECT_EQ(*db_.ZRange("b", 0, -1), (Scored{{"x", 1}, {"y", 2}}));
  EXPECT_EQ(db_.GetTtl("b").remaining, milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Type("b"), KeyType::kNone);
  EXPECT_EQ(*db_.ZAdd("b", {{"fresh", 9}}), 1);
  EXPECT_EQ(*db_.ZRange("b", 0, -1), (Scored{{"fresh", 9}}));
  EXPECT_EQ(db_.GetTtl("b").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, SortedSetsInABatchAndARolledBackBatch) {
  ASSERT_TRUE(db_.ZAdd("z", {{"a", 1}, {"b", 2}}).ok());
  ASSERT_TRUE(*db_.Expire("z", milliseconds(100)));
  Set("text", "abc");
  ASSERT_EQ(db_.Keys().size(), 2);

  WriteBatch ok_batch;
  ok_batch.ZAdd("z", {{"c", 3}, {"a", 10}})
      .ZRem("z", {"b"})
      .ZIncr("z", "c", 0.5);
  ASSERT_TRUE(db_.Write(ok_batch).ok());
  EXPECT_EQ(*db_.ZRange("z", 0, -1), (Scored{{"c", 3.5}, {"a", 10}}));

  WriteBatch bad_batch;
  bad_batch.ZAdd("z", {{"zzz", 99}})
      .ZRem("z", {"a", "c"})
      .ZAdd("u", {{"n", 1}})
      .Incr("text");
  EXPECT_EQ(db_.Write(bad_batch).code(), StatusCode::kNotInteger);
  EXPECT_EQ(*db_.ZRange("z", 0, -1), (Scored{{"c", 3.5}, {"a", 10}}));
  EXPECT_EQ(db_.GetTtl("z").remaining, milliseconds(100));
  EXPECT_EQ(**db_.ZRank("z", "a"), 1);  // the order index was restored too
  EXPECT_EQ(db_.Type("u"), KeyType::kNone);
}

TEST_F(DbTest, DefaultClockConstructs) {
  Db db;
  ASSERT_TRUE(db.Set("k", "v", milliseconds(60000)).ok());
  EXPECT_EQ(db.Get("k"), "v");
}

}  // namespace
}  // namespace qey
