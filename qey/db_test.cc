#include "qey/db.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

#include "gtest/gtest.h"

namespace qey {
namespace {

using std::chrono::milliseconds;

class DbTest : public testing::Test {
 protected:
  DbTest() : db_([this] { return now_; }) {}

  void Advance(milliseconds d) { now_ += d; }

  Db::Clock::time_point now_ = Db::Clock::time_point(milliseconds(1000));
  Db db_;
};

TEST_F(DbTest, GetMissingKeyReturnsNullopt) {
  EXPECT_EQ(db_.Get("missing"), std::nullopt);
}

TEST_F(DbTest, SetThenGet) {
  db_.Set("k", "v");
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, SetOverwritesValue) {
  db_.Set("k", "one");
  db_.Set("k", "two");
  EXPECT_EQ(db_.Get("k"), "two");
}

TEST_F(DbTest, ValuesMayBeEmptyAndContainNul) {
  db_.Set("empty", "");
  db_.Set("bin", std::string_view("a\0b", 3));
  EXPECT_EQ(db_.Get("empty"), "");
  EXPECT_EQ(db_.Get("bin"), std::string("a\0b", 3));
}

TEST_F(DbTest, DelRemovesKey) {
  db_.Set("k", "v");
  EXPECT_TRUE(db_.Del("k"));
  EXPECT_EQ(db_.Get("k"), std::nullopt);
}

TEST_F(DbTest, DelMissingKeyReturnsFalse) { EXPECT_FALSE(db_.Del("nope")); }

TEST_F(DbTest, KeyIsLiveBeforeTtlElapses) {
  db_.Set("k", "v", milliseconds(100));
  Advance(milliseconds(99));
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, KeyExpiresWhenTtlElapses) {
  db_.Set("k", "v", milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Get("k"), std::nullopt);
}

TEST_F(DbTest, DelOnExpiredKeyReturnsFalse) {
  db_.Set("k", "v", milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_FALSE(db_.Del("k"));
}

TEST_F(DbTest, SetWithoutTtlClearsPreviousTtl) {
  db_.Set("k", "v", milliseconds(100));
  db_.Set("k", "v2");
  Advance(milliseconds(1000));
  EXPECT_EQ(db_.Get("k"), "v2");
}

TEST_F(DbTest, ExistsReflectsLiveKeys) {
  EXPECT_FALSE(db_.Exists("k"));
  db_.Set("k", "v", milliseconds(100));
  EXPECT_TRUE(db_.Exists("k"));
  Advance(milliseconds(100));
  EXPECT_FALSE(db_.Exists("k"));
}

TEST_F(DbTest, ExpireSetsTtlOnExistingKey) {
  db_.Set("k", "v");
  EXPECT_TRUE(db_.Expire("k", milliseconds(50)));
  Advance(milliseconds(50));
  EXPECT_EQ(db_.Get("k"), std::nullopt);
}

TEST_F(DbTest, ExpireReplacesExistingTtl) {
  db_.Set("k", "v", milliseconds(50));
  EXPECT_TRUE(db_.Expire("k", milliseconds(500)));
  Advance(milliseconds(100));
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, ExpireMissingKeyReturnsFalse) {
  EXPECT_FALSE(db_.Expire("nope", milliseconds(50)));
}

TEST_F(DbTest, ExpireWithNonPositiveTtlExpiresImmediately) {
  db_.Set("k", "v");
  EXPECT_TRUE(db_.Expire("k", milliseconds(0)));
  EXPECT_FALSE(db_.Exists("k"));
}

TEST_F(DbTest, PersistRemovesTtl) {
  db_.Set("k", "v", milliseconds(50));
  EXPECT_TRUE(db_.Persist("k"));
  Advance(milliseconds(1000));
  EXPECT_EQ(db_.Get("k"), "v");
}

TEST_F(DbTest, PersistReturnsFalseWithoutTtlOrKey) {
  db_.Set("k", "v");
  EXPECT_FALSE(db_.Persist("k"));
  EXPECT_FALSE(db_.Persist("nope"));
}

TEST_F(DbTest, GetTtlReportsState) {
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kMissing);

  db_.Set("k", "v");
  EXPECT_EQ(db_.GetTtl("k").state, Db::TtlState::kNoExpiry);

  db_.Expire("k", milliseconds(100));
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
  db_.Set("n", "10");
  EXPECT_EQ(*db_.Incr("n", 5), 15);
  EXPECT_EQ(*db_.Decr("n", 20), -5);
  EXPECT_EQ(*db_.Decr("n"), -6);
  EXPECT_EQ(db_.Get("n"), "-6");
}

TEST_F(DbTest, IncrPreservesTtl) {
  db_.Set("n", "1", milliseconds(100));
  ASSERT_TRUE(db_.Incr("n").ok());
  Advance(milliseconds(100));
  EXPECT_FALSE(db_.Exists("n"));
}

TEST_F(DbTest, IncrOnExpiredKeyRestartsWithoutTtl) {
  db_.Set("n", "41", milliseconds(100));
  Advance(milliseconds(100));
  EXPECT_EQ(*db_.Incr("n"), 1);
  EXPECT_EQ(db_.GetTtl("n").state, Db::TtlState::kNoExpiry);
}

TEST_F(DbTest, IncrRejectsNonIntegerValues) {
  for (const char* bad : {"", "abc", "1.5", " 1", "1 ", "+1", "0x10"}) {
    db_.Set("n", bad);
    StatusOr<int64_t> r = db_.Incr("n");
    EXPECT_FALSE(r.ok()) << bad;
    EXPECT_EQ(r.status().code(), StatusCode::kNotInteger) << bad;
    EXPECT_EQ(db_.Get("n"), bad);
  }
}

TEST_F(DbTest, IncrRejectsValueOutsideInt64) {
  db_.Set("n", "99999999999999999999");
  EXPECT_EQ(db_.Incr("n").status().code(), StatusCode::kNotInteger);
}

TEST_F(DbTest, IncrDetectsOverflow) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  db_.Set("n", std::to_string(kMax));
  StatusOr<int64_t> r = db_.Incr("n");
  EXPECT_EQ(r.status().code(), StatusCode::kOutOfRange);
  EXPECT_EQ(db_.Get("n"), std::to_string(kMax));
}

TEST_F(DbTest, DecrDetectsUnderflowAndHandlesInt64Min) {
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  db_.Set("n", std::to_string(kMin));
  EXPECT_EQ(db_.Decr("n").status().code(), StatusCode::kOutOfRange);

  db_.Set("n", "-1");
  StatusOr<int64_t> r = db_.Decr("n", kMin);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, std::numeric_limits<int64_t>::max());
}

TEST_F(DbTest, DefaultClockConstructs) {
  Db db;
  db.Set("k", "v", milliseconds(60000));
  EXPECT_EQ(db.Get("k"), "v");
}

}  // namespace
}  // namespace qey
