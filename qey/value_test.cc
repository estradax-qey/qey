#include "qey/value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace qey {
namespace {

TEST(ValueTest, DefaultIsAnEmptyString) {
  Value v;
  EXPECT_EQ(v.type(), KeyType::kString);
  EXPECT_EQ(v.str(), "");
  EXPECT_EQ(v.element_count(), 0);
}

TEST(ValueTest, MakeCreatesEmptyCollectionsOfEachType) {
  EXPECT_EQ(Value::Make(KeyType::kList).type(), KeyType::kList);
  EXPECT_EQ(Value::Make(KeyType::kHash).type(), KeyType::kHash);
  EXPECT_EQ(Value::Make(KeyType::kSet).type(), KeyType::kSet);
  EXPECT_EQ(Value::Make(KeyType::kZSet).type(), KeyType::kZSet);
  EXPECT_EQ(Value::Make(KeyType::kString).type(), KeyType::kString);
}

TEST(ValueTest, CloneIsDeep) {
  Value list = Value::Make(KeyType::kList);
  list.list() = {"a", "b"};
  Value hash = Value::Make(KeyType::kHash);
  hash.hash()["f"] = "v";
  Value set = Value::Make(KeyType::kSet);
  set.set() = {"x", "y"};
  Value zset = Value::Make(KeyType::kZSet);
  zset.zset().Set("m", 1.5);
  Value str("hello");

  Value list2 = list.Clone(), hash2 = hash.Clone(), set2 = set.Clone(),
        zset2 = zset.Clone(), str2 = str.Clone();
  list.list().push_back("c");
  hash.hash()["g"] = "w";
  set.set().insert("z");
  zset.zset().Set("n", 2);
  str.str() += "!";

  EXPECT_EQ(list2.list().size(), 2);
  EXPECT_EQ(hash2.hash().size(), 1);
  EXPECT_EQ(set2.set().size(), 2);
  EXPECT_EQ(zset2.zset().size(), 1);
  EXPECT_EQ(str2.str(), "hello");
  EXPECT_EQ(zset2.zset().order.size(), 1);  // the order index was copied too
}

TEST(ValueTest, ElementCountAndApproxBytes) {
  Value list = Value::Make(KeyType::kList);
  list.list() = {"aa", "b"};
  EXPECT_EQ(list.element_count(), 2);
  EXPECT_EQ(list.ApproxEncodedBytes(), 2 + 4 + 1 + 4);
  EXPECT_EQ(Value("abc").ApproxEncodedBytes(), 3);
}

TEST(ZSetTest, KeepsMembersOrderedByScoreThenName) {
  ZSet z;
  EXPECT_TRUE(z.Set("b", 2));
  EXPECT_TRUE(z.Set("a", 2));
  EXPECT_TRUE(z.Set("c", 1));
  EXPECT_FALSE(z.Set("b", 0));  // moves an existing member
  std::vector<std::string> in_order;
  for (const auto& [score, member] : z.order) in_order.push_back(member);
  EXPECT_EQ(in_order, (std::vector<std::string>{"b", "c", "a"}));
  EXPECT_EQ(z.size(), 3);
  EXPECT_EQ(z.order.size(), 3);
}

TEST(ZSetTest, SettingTheSameScoreChangesNothingAndRemoveCleansBoth) {
  ZSet z;
  z.Set("a", 1);
  EXPECT_FALSE(z.Set("a", 1));
  EXPECT_EQ(z.order.size(), 1);
  EXPECT_TRUE(z.Remove("a"));
  EXPECT_FALSE(z.Remove("a"));
  EXPECT_EQ(z.size(), 0);
  EXPECT_EQ(z.order.size(), 0);
}

TEST(StreamIdTest, OrdersByMillisecondsThenSequence) {
  EXPECT_LT((StreamId{1, 9}), (StreamId{2, 0}));
  EXPECT_LT((StreamId{2, 0}), (StreamId{2, 1}));
  EXPECT_EQ((StreamId{2, 1}), (StreamId{2, 1}));
}

TEST(StreamIdTest, NextIsTheSmallestGreaterId) {
  EXPECT_EQ((StreamId{5, 3}).Next(), (StreamId{5, 4}));
  const uint64_t top = INT64_MAX;
  EXPECT_EQ((StreamId{5, top}).Next(), (StreamId{6, 0}));
  // The largest ID has no successor.
  EXPECT_EQ((StreamId{top, top}).Next(), (StreamId{top, top}));
}

TEST(StreamIdTest, ParseAndToStringRoundTrip) {
  EXPECT_EQ((StreamId{1760000000000, 7}).ToString(), "1760000000000-7");
  EXPECT_EQ(StreamId::Parse("1760000000000-7"), (StreamId{1760000000000, 7}));
  EXPECT_EQ(StreamId::Parse("12"), (StreamId{12, 0}));  // "ms" means ms-0
  EXPECT_EQ(StreamId::Parse("0-0"), (StreamId{0, 0}));
  const StreamId top{INT64_MAX, INT64_MAX};
  EXPECT_EQ(StreamId::Parse(top.ToString()), top);
}

TEST(StreamIdTest, ParseRejectsAnythingElse) {
  for (const char* bad :
       {"", "-", "1-", "-1", "-1-2", "+1", "1-+2", " 1", "1 ", "1-2-3", "a",
        "1-b", "0x10", "1.5", "9223372036854775808", "1-9223372036854775808",
        "99999999999999999999999"}) {
    EXPECT_EQ(StreamId::Parse(bad), std::nullopt) << bad;
  }
}

TEST(ValueTest, StreamsMakeCloneAndCount) {
  Value v = Value::Make(KeyType::kStream);
  EXPECT_EQ(v.type(), KeyType::kStream);
  EXPECT_EQ(v.element_count(), 0);
  v.stream().entries.push_back({{5, 0}, "a"});
  v.stream().entries.push_back({{5, 1}, "bc"});
  v.stream().last_id = {5, 1};
  EXPECT_EQ(v.element_count(), 2);
  EXPECT_GE(v.ApproxEncodedBytes(), 3);
  Value copy = v.Clone();
  v.stream().entries.clear();  // the copy is independent
  EXPECT_EQ(copy.type(), KeyType::kStream);
  ASSERT_EQ(copy.stream().entries.size(), 2);
  EXPECT_EQ(copy.stream().entries[1].payload, "bc");
  EXPECT_EQ(copy.stream().last_id, (StreamId{5, 1}));
}

}  // namespace
}  // namespace qey
