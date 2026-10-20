#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "qey/db.h"

namespace qey {
namespace {

using std::chrono::milliseconds;

// A store whose clocks the test controls. Entry IDs come from the wall clock.
class StreamTest : public testing::Test {
 protected:
  StreamTest() : db_([this] { return steady_; }, [this] { return wall_; }) {}

  void SetWallMs(int64_t ms) {
    wall_ = std::chrono::system_clock::time_point(milliseconds(ms));
  }

  StreamId Add(std::string_view key, std::string_view payload) {
    StatusOr<StreamId> id = db_.XAdd(key, payload);
    EXPECT_TRUE(id.ok()) << id.status().message();
    return id.ok() ? *id : StreamId();
  }

  // The payloads of every entry, oldest first.
  std::vector<std::string> Payloads(std::string_view key) {
    StatusOr<std::vector<StreamEntry>> all =
        db_.XRange(key, StreamId(), std::numeric_limits<size_t>::max());
    EXPECT_TRUE(all.ok());
    std::vector<std::string> out;
    for (const StreamEntry& e : *all) out.push_back(e.payload);
    return out;
  }

  Db::Clock::time_point steady_ = Db::Clock::time_point(milliseconds(1000));
  std::chrono::system_clock::time_point wall_ =
      std::chrono::system_clock::time_point(milliseconds(5000));
  Db db_;
};

using Strings = std::vector<std::string>;

TEST_F(StreamTest, IdsStartWithTheTimeAndCountUpWithinAMillisecond) {
  EXPECT_EQ(Add("s", "a"), (StreamId{5000, 0}));
  EXPECT_EQ(Add("s", "b"), (StreamId{5000, 1}));
  EXPECT_EQ(Add("s", "c"), (StreamId{5000, 2}));
  SetWallMs(5001);
  EXPECT_EQ(Add("s", "d"), (StreamId{5001, 0}));
  EXPECT_EQ(db_.Type("s"), KeyType::kStream);
  EXPECT_EQ(*db_.XLen("s"), 4);
  EXPECT_EQ(Payloads("s"), (Strings{"a", "b", "c", "d"}));
}

TEST_F(StreamTest, IdsKeepGrowingWhenTheClockGoesBackwards) {
  EXPECT_EQ(Add("s", "a"), (StreamId{5000, 0}));
  SetWallMs(100);  // the clock jumps back
  EXPECT_EQ(Add("s", "b"), (StreamId{5000, 1}));
  EXPECT_EQ(Add("s", "c"), (StreamId{5000, 2}));
  SetWallMs(5000);
  EXPECT_EQ(Add("s", "d"), (StreamId{5000, 3}));
  SetWallMs(5002);  // and when it catches up, time takes over again
  EXPECT_EQ(Add("s", "e"), (StreamId{5002, 0}));
}

TEST_F(StreamTest, StreamsHaveSeparateIds) {
  Add("a", "x");
  Add("a", "x");
  EXPECT_EQ(Add("b", "x"), (StreamId{5000, 0}));
}

TEST_F(StreamTest, MissingKeyReadsAsEmpty) {
  EXPECT_EQ(*db_.XLen("none"), 0);
  EXPECT_EQ(*db_.XLastId("none"), std::nullopt);
  EXPECT_TRUE(db_.XRange("none", StreamId(), 10)->empty());
  EXPECT_TRUE(db_.XRevRange("none", 10)->empty());
  EXPECT_EQ(*db_.XTrimMaxLen("none", 1), 0);
  EXPECT_EQ(*db_.XTrimMinId("none", StreamId{9, 9}), 0);
  EXPECT_EQ(db_.Type("none"), KeyType::kNone);  // none of it made the key
}

TEST_F(StreamTest, XLastIdIsTheNewestId) {
  Add("s", "a");
  const StreamId second = Add("s", "b");
  EXPECT_EQ(*db_.XLastId("s"), std::optional<StreamId>(second));
}

TEST_F(StreamTest, PayloadsAreOpaqueBytes) {
  const std::string binary("\0\xff{\"role\":\"user\"}\n", 17);
  Add("s", binary);
  Add("s", "");
  EXPECT_EQ(Payloads("s"), (Strings{binary, ""}));
}

TEST_F(StreamTest, XRangeReadsFromAnIdUpToACount) {
  const StreamId a = Add("s", "a");
  SetWallMs(5001);
  const StreamId b = Add("s", "b");
  const StreamId c = Add("s", "c");
  SetWallMs(5002);
  const StreamId d = Add("s", "d");

  auto ids = [&](StatusOr<std::vector<StreamEntry>> r) {
    std::vector<StreamId> out;
    for (const StreamEntry& e : *r) out.push_back(e.id);
    return out;
  };
  using Ids = std::vector<StreamId>;
  EXPECT_EQ(ids(db_.XRange("s", StreamId(), 100)), (Ids{a, b, c, d}));
  EXPECT_EQ(ids(db_.XRange("s", StreamId(), 2)), (Ids{a, b}));
  EXPECT_EQ(ids(db_.XRange("s", StreamId(), 0)), (Ids{}));
  EXPECT_EQ(ids(db_.XRange("s", b, 100)), (Ids{b, c, d}));  // inclusive
  EXPECT_EQ(ids(db_.XRange("s", b.Next(), 100)), (Ids{c, d}));
  EXPECT_EQ(ids(db_.XRange("s", b, 100, c)), (Ids{b, c}));  // `to` is too
  EXPECT_EQ(ids(db_.XRange("s", b, 100, b)), (Ids{b}));
  EXPECT_EQ(ids(db_.XRange("s", c, 100, b)), (Ids{}));
  // Between two entries, and past the end.
  EXPECT_EQ(ids(db_.XRange("s", StreamId{5001, 5}, 100)), (Ids{d}));
  EXPECT_EQ(ids(db_.XRange("s", d.Next(), 100)), (Ids{}));
}

TEST_F(StreamTest, XRangeFromATimeReadsTheMessagesSinceThen) {
  Add("s", "old");
  SetWallMs(6000);
  Add("s", "new1");
  Add("s", "new2");
  EXPECT_EQ(db_.XRange("s", StreamId{6000, 0}, 10)->size(), 2);
  EXPECT_EQ(db_.XRange("s", StreamId{5001, 0}, 10, StreamId{5999, 0})->size(),
            0);
}

TEST_F(StreamTest, ResumingWithNextPagesThroughEveryEntryOnce) {
  for (int i = 0; i < 25; ++i) Add("s", std::to_string(i));
  std::vector<std::string> seen;
  StreamId cursor;
  for (;;) {
    StatusOr<std::vector<StreamEntry>> page = db_.XRange("s", cursor, 7);
    ASSERT_TRUE(page.ok());
    if (page->empty()) break;
    for (const StreamEntry& e : *page) seen.push_back(e.payload);
    cursor = page->back().id.Next();
  }
  ASSERT_EQ(seen.size(), 25);
  for (int i = 0; i < 25; ++i) EXPECT_EQ(seen[i], std::to_string(i));
}

TEST_F(StreamTest, XRevRangeReadsTheNewestFirst) {
  std::vector<StreamId> ids;
  for (int i = 0; i < 5; ++i) ids.push_back(Add("s", std::to_string(i)));
  auto payloads = [](StatusOr<std::vector<StreamEntry>> r) {
    Strings out;
    for (const StreamEntry& e : *r) out.push_back(e.payload);
    return out;
  };
  EXPECT_EQ(payloads(db_.XRevRange("s", 2)), (Strings{"4", "3"}));
  EXPECT_EQ(payloads(db_.XRevRange("s", 100)),
            (Strings{"4", "3", "2", "1", "0"}));
  EXPECT_EQ(payloads(db_.XRevRange("s", 0)), (Strings{}));
  // `before` is inclusive.
  EXPECT_EQ(payloads(db_.XRevRange("s", 2, ids[2])), (Strings{"2", "1"}));
  EXPECT_EQ(payloads(db_.XRevRange("s", 5, ids[0])), (Strings{"0"}));
  EXPECT_EQ(payloads(db_.XRevRange("s", 5, StreamId{1, 0})), (Strings{}));
  EXPECT_EQ(payloads(db_.XRevRange("s", 1, StreamId{9999, 0})), (Strings{"4"}));
}

TEST_F(StreamTest, XAddWithMaxLenKeepsTheNewestEntries) {
  for (int i = 0; i < 3; ++i)
    ASSERT_TRUE(db_.XAdd("s", std::to_string(i), 3).ok());
  EXPECT_EQ(Payloads("s"), (Strings{"0", "1", "2"}));
  StatusOr<StreamId> id = db_.XAdd("s", "3", 3);
  ASSERT_TRUE(id.ok());
  EXPECT_EQ(Payloads("s"), (Strings{"1", "2", "3"}));
  EXPECT_EQ(*db_.XLastId("s"), std::optional<StreamId>(*id));
  ASSERT_TRUE(db_.XAdd("s", "4", 1).ok());
  EXPECT_EQ(Payloads("s"), (Strings{"4"}));
}

TEST_F(StreamTest, XAddWithMaxLenZeroLeavesAnEmptyStreamThatRemembersItsId) {
  StatusOr<StreamId> id = db_.XAdd("s", "gone", 0);
  ASSERT_TRUE(id.ok());
  EXPECT_EQ(*db_.XLen("s"), 0);
  EXPECT_EQ(db_.Type("s"), KeyType::kStream);  // still there
  EXPECT_EQ(*db_.XLastId("s"), std::optional<StreamId>(*id));
  SetWallMs(1);  // even with the clock far behind
  EXPECT_GT(Add("s", "next"), *id);
}

TEST_F(StreamTest, XTrimMaxLenRemovesTheOldestAndCountsThem) {
  for (int i = 0; i < 5; ++i) Add("s", std::to_string(i));
  EXPECT_EQ(*db_.XTrimMaxLen("s", 10), 0);  // already within the limit
  EXPECT_EQ(*db_.XTrimMaxLen("s", 5), 0);
  EXPECT_EQ(*db_.XTrimMaxLen("s", 2), 3);
  EXPECT_EQ(Payloads("s"), (Strings{"3", "4"}));
  EXPECT_EQ(*db_.XTrimMaxLen("s", 0), 2);
  EXPECT_EQ(*db_.XLen("s"), 0);
  EXPECT_EQ(*db_.XTrimMaxLen("s", 0), 0);
}

TEST_F(StreamTest, XTrimMinIdRemovesEntriesBelowIt) {
  const StreamId a = Add("s", "a");
  const StreamId b = Add("s", "b");
  const StreamId c = Add("s", "c");
  EXPECT_EQ(*db_.XTrimMinId("s", StreamId()), 0);
  EXPECT_EQ(*db_.XTrimMinId("s", a), 0);  // `a` itself is kept
  EXPECT_EQ(*db_.XTrimMinId("s", b), 1);
  EXPECT_EQ(Payloads("s"), (Strings{"b", "c"}));
  EXPECT_EQ(*db_.XTrimMinId("s", c.Next()), 2);  // just past the newest
  EXPECT_EQ(*db_.XLen("s"), 0);
}

TEST_F(StreamTest, XTrimMinIdPastTheNewestEmptiesButKeepsTheLastId) {
  Add("s", "a");
  const StreamId last = Add("s", "b");
  SetWallMs(1000000);
  EXPECT_EQ(*db_.XTrimMinId("s", StreamId{999999999, 0}), 2);
  EXPECT_EQ(*db_.XLen("s"), 0);
  // Trimming beyond the end does not move the last ID forward.
  EXPECT_EQ(*db_.XLastId("s"), std::optional<StreamId>(last));
  EXPECT_EQ(Add("s", "c"), (StreamId{1000000, 0}));
}

TEST_F(StreamTest, XTrimMinIdRejectsPartsAboveInt64Max) {
  Add("s", "a");
  const uint64_t too_big = static_cast<uint64_t>(INT64_MAX) + 1;
  EXPECT_EQ(db_.XTrimMinId("s", StreamId{too_big, 0}).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.XTrimMinId("s", StreamId{0, too_big}).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(*db_.XLen("s"), 1);
}

TEST_F(StreamTest, OtherTypesAndStreamsDoNotMix) {
  ASSERT_TRUE(db_.Set("str", "v").ok());
  ASSERT_TRUE(db_.RPush("list", {"a"}).ok());
  for (const char* key : {"str", "list"}) {
    EXPECT_EQ(db_.XAdd(key, "x").status().code(), StatusCode::kWrongType);
    EXPECT_EQ(db_.XAdd(key, "x", 5).status().code(), StatusCode::kWrongType);
    EXPECT_EQ(db_.XLen(key).status().code(), StatusCode::kWrongType);
    EXPECT_EQ(db_.XLastId(key).status().code(), StatusCode::kWrongType);
    EXPECT_EQ(db_.XRange(key, StreamId(), 1).status().code(),
              StatusCode::kWrongType);
    EXPECT_EQ(db_.XRevRange(key, 1).status().code(), StatusCode::kWrongType);
    EXPECT_EQ(db_.XTrimMaxLen(key, 1).status().code(), StatusCode::kWrongType);
    EXPECT_EQ(db_.XTrimMinId(key, StreamId()).status().code(),
              StatusCode::kWrongType);
  }
  EXPECT_EQ(db_.Get("str"), "v");  // unchanged

  Add("s", "a");
  EXPECT_EQ(db_.Get("s"), std::nullopt);  // not a string
  EXPECT_EQ(db_.Append("s", "x").status().code(), StatusCode::kWrongType);
  EXPECT_EQ(db_.LLen("s").status().code(), StatusCode::kWrongType);
  ASSERT_TRUE(db_.Set("s", "plain").ok());  // Set replaces whatever was there
  EXPECT_EQ(db_.Type("s"), KeyType::kString);
}

TEST_F(StreamTest, DelRenameKeysAndExpiryWorkOnStreams) {
  Add("a", "1");
  const StreamId second = Add("a", "2");
  EXPECT_TRUE(*db_.Rename("a", "b"));
  EXPECT_EQ(db_.Type("a"), KeyType::kNone);
  EXPECT_EQ(Payloads("b"), (Strings{"1", "2"}));
  EXPECT_EQ(*db_.XLastId("b"), std::optional<StreamId>(second));
  EXPECT_EQ(db_.Keys("b"), Strings{"b"});

  ASSERT_TRUE(*db_.Expire("b", milliseconds(100)));
  steady_ += milliseconds(100);
  EXPECT_EQ(db_.Type("b"), KeyType::kNone);
  EXPECT_EQ(*db_.XLen("b"), 0);

  Add("c", "x");
  EXPECT_TRUE(*db_.Del("c"));
  EXPECT_EQ(db_.Type("c"), KeyType::kNone);
}

TEST_F(StreamTest, AnExpiredStreamStartsOverWithFreshIds) {
  Add("s", "a");
  Add("s", "b");
  ASSERT_TRUE(*db_.Expire("s", milliseconds(10)));
  steady_ += milliseconds(10);
  SetWallMs(7000);
  EXPECT_EQ(Add("s", "c"), (StreamId{7000, 0}));
  EXPECT_EQ(Payloads("s"), Strings{"c"});
}

TEST_F(StreamTest, DelForgetsTheLastId) {
  Add("s", "a");
  ASSERT_TRUE(*db_.Del("s"));
  EXPECT_EQ(*db_.XLastId("s"), std::nullopt);
  EXPECT_EQ(Add("s", "b"), (StreamId{5000, 0}));
}

TEST_F(StreamTest, LimitsOnKeysAndPayloads) {
  const std::string big_payload(Db::kMaxValueSize + 1, 'x');
  const std::string big_key(Db::kMaxKeySize + 1, 'k');
  EXPECT_EQ(db_.XAdd("s", big_payload).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.XAdd("s", big_payload, 5).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.XAdd(big_key, "x").status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(db_.Type("s"), KeyType::kNone);  // nothing was changed
}

TEST_F(StreamTest, ConcurrentAddsGetUniqueAscendingIdsAndReadersSeeOrder) {
  constexpr int kThreads = 4;
  constexpr int kPerThread = 250;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this] {
      for (int i = 0; i < kPerThread; ++i) {
        EXPECT_TRUE(db_.XAdd("s", "x").ok());
        auto page = db_.XRevRange("s", 5);
        EXPECT_TRUE(page.ok());
        for (size_t j = 1; j < page->size(); ++j) {
          EXPECT_GT((*page)[j - 1].id, (*page)[j].id);
        }
      }
    });
  }
  for (std::thread& t : threads) t.join();
  StatusOr<std::vector<StreamEntry>> all =
      db_.XRange("s", StreamId(), std::numeric_limits<size_t>::max());
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all->size(), kThreads * kPerThread);
  for (size_t i = 1; i < all->size(); ++i) {
    EXPECT_LT((*all)[i - 1].id, (*all)[i].id);
  }
}

}  // namespace
}  // namespace qey
