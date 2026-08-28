#include "workload.h"

#include <cstdint>
#include <initializer_list>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace kvlog::bench {
namespace {

TEST(WorkloadTest, KeysSortNumericallyAndMissingKeysFallBetweenThem) {
  EXPECT_EQ("0000000000000042", BenchKey(42));
  for (uint64_t i : {0ULL, 9ULL, 99ULL, 123456789ULL}) {
    EXPECT_LT(BenchKey(i), BenchKey(i + 1));
    EXPECT_LT(BenchKey(i), MissingKey(i));
    EXPECT_LT(MissingKey(i), BenchKey(i + 1));
  }
}

TEST(WorkloadTest, ScatteredRangeVisitsEveryIndexOnce) {
  for (uint64_t count : {1ULL, 2ULL, 7ULL, 1000ULL, 65536ULL, 99991ULL}) {
    const ScatteredRange range = ScatteredRange::Scattered(500, count, 12345);
    std::vector<bool> seen(count, false);
    uint64_t in_order = 0;
    for (uint64_t i = 0; i < count; ++i) {
      const uint64_t index = range.At(i);
      ASSERT_GE(index, 500u);
      ASSERT_LT(index, 500 + count);
      ASSERT_FALSE(seen[index - 500]) << "count " << count << " index " << index;
      seen[index - 500] = true;
      in_order += index == 500 + i ? 1 : 0;
    }
    if (count >= 1000) {
      EXPECT_LT(in_order, count / 100) << "the order is not scattered";
    }
  }
  const ScatteredRange sequential = ScatteredRange::Sequential(10, 5);
  for (uint64_t i = 0; i < 5; ++i) {
    EXPECT_EQ(10 + i, sequential.At(i));
  }
}

TEST(WorkloadTest, ScatteredRangeHandlesTheLargestCount) {
  constexpr uint64_t kCount = 1ULL << 32;
  const ScatteredRange range = ScatteredRange::Scattered(0, kCount, ~0ULL);
  std::set<uint64_t> seen;
  // Spelled out as uint64_t: on Linux it is unsigned long, not unsigned long long.
  const std::initializer_list<uint64_t> indexes = {0, 1, kCount / 2, kCount - 2, kCount - 1};
  for (const uint64_t i : indexes) {
    const uint64_t index = range.At(i);
    EXPECT_LT(index, kCount);
    EXPECT_TRUE(seen.insert(index).second);
  }
}

TEST(WorkloadTest, ZipfianIsSkewedTowardLowRanks) {
  constexpr uint64_t kItems = 10000;
  constexpr int kDraws = 200000;
  ZipfianGenerator zipf(kItems);
  std::mt19937_64 rng(1);
  std::vector<int> hits(kItems, 0);
  for (int i = 0; i < kDraws; ++i) {
    const uint64_t rank = zipf.Next(&rng);
    ASSERT_LT(rank, kItems);
    ++hits[rank];
  }
  // With theta 0.99 and 10^4 items, rank 0 draws about 10% and the top 1%
  // of ranks well over half of all draws.
  EXPECT_GT(hits[0], kDraws / 20);
  EXPECT_GT(hits[0], hits[1]);
  EXPECT_GT(hits[1], hits[10]);
  int top = 0;
  for (uint64_t r = 0; r < kItems / 100; ++r) {
    top += hits[r];
  }
  EXPECT_GT(top, kDraws / 2);
}

TEST(WorkloadTest, GrowingZipfianMatchesOneBuiltAtTheFinalSize) {
  ZipfianGenerator grown(100);
  grown.Grow(5000);
  grown.Grow(20000);
  ZipfianGenerator direct(20000);
  std::mt19937_64 a(7);
  std::mt19937_64 b(7);
  int same = 0;
  for (int i = 0; i < 1000; ++i) {
    same += grown.Next(&a) == direct.Next(&b) ? 1 : 0;
  }
  // Summation order differs, so a draw may land one rank apart at a
  // boundary; essentially all must agree.
  EXPECT_GE(same, 990);
  EXPECT_EQ(20000u, grown.items());
}

TEST(WorkloadTest, KeyChooserStaysInRange) {
  for (bool zipfian : {false, true}) {
    KeyChooser chooser(zipfian, 10);
    std::mt19937_64 rng(3);
    std::set<uint64_t> seen;
    for (int i = 0; i < 2000; ++i) {
      const uint64_t index = chooser.Next(&rng);
      ASSERT_LT(index, 10u);
      seen.insert(index);
    }
    EXPECT_GT(seen.size(), 5u) << "zipfian " << zipfian;
    chooser.Grow(1000);
    for (int i = 0; i < 2000; ++i) {
      ASSERT_LT(chooser.Next(&rng), 1000u);
    }
  }
}

TEST(WorkloadTest, PercentilesUseNearestRank) {
  LatencyRecorder recorder;
  EXPECT_EQ(0, recorder.Percentile(50));
  for (int i = 100; i >= 1; --i) {
    recorder.Add(i);
  }
  EXPECT_EQ(50, recorder.Percentile(50));
  EXPECT_EQ(99, recorder.Percentile(99));
  EXPECT_EQ(100, recorder.Percentile(100));
  EXPECT_EQ(1, recorder.Percentile(0));
}

}  // namespace
}  // namespace kvlog::bench
