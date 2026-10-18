#include "qey/db.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "gtest/gtest.h"

namespace qey {
namespace {

using std::chrono::milliseconds;

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

TEST_F(DbTest, DefaultClockConstructs) {
  Db db;
  ASSERT_TRUE(db.Set("k", "v", milliseconds(60000)).ok());
  EXPECT_EQ(db.Get("k"), "v");
}

}  // namespace
}  // namespace qey
