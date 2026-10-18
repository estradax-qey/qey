#include "bench/measure.h"

#include <chrono>
#include <thread>
#include <vector>

#include "bench/process.h"
#include "gtest/gtest.h"

namespace qey::bench {
namespace {

TEST(PercentileTest, NearestRank) {
  std::vector<double> v;
  for (int i = 1; i <= 100; ++i) v.push_back(i);
  EXPECT_EQ(Percentile(v, 50), 50);
  EXPECT_EQ(Percentile(v, 99), 99);
  EXPECT_EQ(Percentile(v, 99.9), 100);
  EXPECT_EQ(Percentile(v, 100), 100);
  EXPECT_EQ(Percentile(v, 0), 1);
  EXPECT_EQ(Percentile(v, 1), 1);
  EXPECT_EQ(Percentile({7}, 50), 7);
  EXPECT_EQ(Percentile({7}, 99.9), 7);
  EXPECT_EQ(Percentile({1, 2}, 50), 1);
  EXPECT_EQ(Percentile({1, 2}, 51), 2);
}

TEST(MeasureTest, CountsEveryOperationAndSamplesSome) {
  int calls = 0;
  Timing t = Measure(100, 10, [&](size_t) { ++calls; });
  EXPECT_EQ(calls, 100);
  EXPECT_EQ(t.ops, 100);
  EXPECT_EQ(t.sampled, 10);
  EXPECT_GT(t.ops_per_second, 0);
  EXPECT_LE(t.p50_ns, t.p99_ns);
  EXPECT_LE(t.p99_ns, t.max_ns);
}

TEST(MeasureTest, SamplingEveryCallGivesALatencyForEachOne) {
  Timing t = Measure(5, 1, [](size_t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  });
  EXPECT_EQ(t.sampled, 5);
  EXPECT_GE(t.p50_ns, 2e6);  // at least the sleep
  EXPECT_GE(t.mean_ns, 2e6);
  EXPECT_GE(t.seconds, 0.01);
}

TEST(MeasureTest, ZeroOperationsIsHarmless) {
  Timing t = Measure(0, 1, [](size_t) { FAIL(); });
  EXPECT_EQ(t.ops, 0);
  EXPECT_EQ(t.sampled, 0);
}

TEST(ProcessTest, ReadsItsOwnMemory) {
  EXPECT_GT(CurrentFootprintBytes(),
            1u << 20);  // a test binary is over a megabyte
  EXPECT_GE(PeakFootprintBytes(), CurrentFootprintBytes() / 2);
}

TEST(ProcessTest, ResidentMemoryGrowsWithTouchedPages) {
  const uint64_t before = CurrentFootprintBytes();
  std::vector<char> big(64u << 20);
  for (size_t i = 0; i < big.size(); i += 4096) big[i] = 1;  // touch each page
  Consume(big);
  EXPECT_GT(CurrentFootprintBytes(), before + (32u << 20));
}

TEST(ChildTest, MeasuresMemoryThatTheChildAllocates) {
  std::optional<MemoryUse> use =
      MeasureInChild([](const auto& begin, const auto& end) {
        begin();
        std::vector<char> big(64u << 20);
        for (size_t i = 0; i < big.size(); i += 4096) big[i] = 1;
        Consume(big);
        end();
      });
  ASSERT_TRUE(use.has_value());
  EXPECT_GT(use->before, 0u);
  EXPECT_GT(use->after, use->before + (32u << 20));
  EXPECT_GE(use->peak, use->after);
}

TEST(ChildTest, WorkDoneBeforeBeginIsNotCounted) {
  std::optional<MemoryUse> use =
      MeasureInChild([](const auto& begin, const auto& end) {
        std::vector<char> setup(64u << 20);
        for (size_t i = 0; i < setup.size(); i += 4096) setup[i] = 1;
        Consume(setup);
        begin();  // the 64 MB above is part of the baseline
        std::vector<char> small(1u << 20);
        for (size_t i = 0; i < small.size(); i += 4096) small[i] = 1;
        Consume(small);
        end();
      });
  ASSERT_TRUE(use.has_value());
  EXPECT_GT(use->before, 60u << 20);
  // Signed: a sanitizer's runtime can release memory in between.
  EXPECT_LT(
      static_cast<int64_t>(use->after) - static_cast<int64_t>(use->before),
      16 << 20);
}

TEST(ChildTest, MemoryIsReadWhileTheDataIsStillAliveNotAfterItIsFreed) {
  // The data lives in a scope that ends before the work returns. A reading
  // taken when the work returns would find none of it.
  std::optional<MemoryUse> use =
      MeasureInChild([](const auto& begin, const auto& end) {
        begin();
        {
          std::vector<char> big(64u << 20);
          for (size_t i = 0; i < big.size(); i += 4096) big[i] = 1;
          Consume(big);
          end();
        }  // freed here
      });
  ASSERT_TRUE(use.has_value());
  EXPECT_GT(use->after, use->before + (32u << 20));
}

TEST(ChildTest, WorkThatDoesNotCallBothBeginAndEndIsAFailure) {
  EXPECT_FALSE(MeasureInChild([](const auto&, const auto&) {}).has_value());
  EXPECT_FALSE(MeasureInChild([](const auto& begin, const auto&) {
                 begin();
               }).has_value());
  EXPECT_FALSE(
      MeasureInChild([](const auto&, const auto& end) { end(); }).has_value());
}

TEST(ChildTest, AChildThatCrashesIsAFailureNotACrashOfTheParent) {
  EXPECT_FALSE(MeasureInChild([](const auto& begin, const auto&) {
                 begin();
                 std::abort();
               }).has_value());
}

}  // namespace
}  // namespace qey::bench
