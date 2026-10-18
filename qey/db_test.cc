#include "qey/db.h"

#include <chrono>
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

TEST_F(DbTest, DefaultClockConstructs) {
  Db db;
  db.Set("k", "v", milliseconds(60000));
  EXPECT_EQ(db.Get("k"), "v");
}

}  // namespace
}  // namespace qey
