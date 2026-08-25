#include "util/bloom.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test_util.h"
#include "util/coding.h"

namespace kvlog {
namespace {

std::string BuildFilter(const std::vector<std::string>& keys, int bits_per_key) {
  std::vector<uint64_t> hashes;
  hashes.reserve(keys.size());
  for (const auto& key : keys) {
    hashes.push_back(BloomHash(key));
  }
  std::string filter;
  BuildBloomFilter(hashes, bits_per_key, &filter);
  return filter;
}

// Key shapes seen in practice. Each maps an index to a distinct key; the
// keys of index >= the number added are the absent probes.
struct KeyShape {
  const char* name;
  std::function<std::string(uint64_t)> make;
};

std::string BigEndian64(uint64_t i) {
  std::string s(8, '\0');
  for (int b = 0; b < 8; ++b) {
    s[static_cast<size_t>(b)] = static_cast<char>(i >> (56 - 8 * b));
  }
  return s;
}

std::string RandomKey(uint64_t i, size_t min_len, size_t max_len) {
  std::mt19937_64 rng(i * 0x9e3779b97f4a7c15ull + 1);
  const size_t len = min_len + rng() % (max_len - min_len + 1);
  // The index is appended so keys stay distinct even when short.
  return test::RandomBytes(&rng, len) + BigEndian64(i);
}

const std::vector<KeyShape>& KeyShapes() {
  static const std::vector<KeyShape> shapes = {
      {"decimal", [](uint64_t i) { return test::Key(i); }},
      {"little_endian_int", [](uint64_t i) {
         std::string s;
         PutFixed64(&s, i);
         return s;
       }},
      {"big_endian_int", BigEndian64},
      {"random_binary", [](uint64_t i) { return RandomKey(i, 0, 24); }},
      {"long_common_prefix", [](uint64_t i) {
         return std::string(200, 'p') + "/" + std::to_string(i);
       }},
      {"varint", [](uint64_t i) {
         std::string s;
         PutVarint64(&s, i);
         return s;
       }},
  };
  return shapes;
}

TEST(BloomTest, ProbeCountFollowsBitsPerKey) {
  EXPECT_EQ(1, BloomProbesForBitsPerKey(1));  // round(0.69)
  EXPECT_EQ(1, BloomProbesForBitsPerKey(2));  // round(1.39)
  EXPECT_EQ(2, BloomProbesForBitsPerKey(3));  // round(2.08)
  EXPECT_EQ(6, BloomProbesForBitsPerKey(8));  // round(5.55)
  EXPECT_EQ(7, BloomProbesForBitsPerKey(10)); // round(6.93)
  EXPECT_EQ(11, BloomProbesForBitsPerKey(16));
  EXPECT_EQ(30, BloomProbesForBitsPerKey(44));  // round(30.5) clamped
  EXPECT_EQ(30, BloomProbesForBitsPerKey(1000));
  EXPECT_EQ(1, BloomProbesForBitsPerKey(0));
}

TEST(BloomTest, EncodingIsBitArrayThenProbeCount) {
  const std::string filter = BuildFilter({"a", "b", "c"}, 10);
  // 3 keys * 10 bits is below the 64-bit floor.
  ASSERT_EQ(8u + 1u, filter.size());
  EXPECT_EQ(7, filter.back());

  std::vector<std::string> keys;
  for (int i = 0; i < 1000; ++i) {
    keys.push_back(test::Key(static_cast<uint64_t>(i)));
  }
  EXPECT_EQ(1000u * 10 / 8 + 1, BuildFilter(keys, 10).size());
  EXPECT_EQ((1000u * 7 + 7) / 8 + 1, BuildFilter(keys, 7).size());
}

TEST(BloomTest, EmptyFilterMatchesNothingAddedButStillParses) {
  const std::string filter = BuildFilter({}, 10);
  BloomFilterReader reader(filter);
  int matches = 0;
  for (int i = 0; i < 1000; ++i) {
    matches += reader.KeyMayMatch(test::Key(static_cast<uint64_t>(i))) ? 1 : 0;
  }
  EXPECT_EQ(0, matches);
}

TEST(BloomTest, MalformedFiltersMatchEverything) {
  for (const std::string& filter :
       {std::string(), std::string(1, '\x07'), std::string(8, '\0') + '\x00',
        std::string(8, '\0') + '\x1f', std::string(8, '\0') + '\xff'}) {
    BloomFilterReader reader(filter);
    EXPECT_TRUE(reader.KeyMayMatch("anything")) << filter.size();
    EXPECT_TRUE(reader.KeyMayMatch("")) << filter.size();
  }
  BloomFilterReader default_reader;
  EXPECT_TRUE(default_reader.KeyMayMatch("x"));
}

TEST(BloomTest, SmallFiltersHoldTheirKeys) {
  for (int n = 1; n <= 64; ++n) {
    std::vector<std::string> keys;
    for (int i = 0; i < n; ++i) {
      keys.push_back("k" + std::to_string(i));
    }
    const std::string filter = BuildFilter(keys, 10);
    BloomFilterReader reader(filter);
    for (const auto& key : keys) {
      ASSERT_TRUE(reader.KeyMayMatch(key)) << n << " " << key;
    }
  }
}

// The two properties the database relies on, over 100k keys of several
// shapes: no added key is ever reported absent, and at 10 bits per key
// fewer than 2% of absent keys are reported present (theory: about 0.8%).
TEST(BloomTest, NoFalseNegativesAndLowFalsePositiveRateForEveryKeyShape) {
  constexpr uint64_t kKeys = 100000;
  constexpr uint64_t kProbes = 100000;
  for (const KeyShape& shape : KeyShapes()) {
    std::vector<std::string> keys;
    keys.reserve(kKeys);
    for (uint64_t i = 0; i < kKeys; ++i) {
      keys.push_back(shape.make(i));
    }
    const std::string filter = BuildFilter(keys, 10);
    BloomFilterReader reader(filter);
    for (const auto& key : keys) {
      ASSERT_TRUE(reader.KeyMayMatch(key)) << shape.name << ": false negative";
    }
    uint64_t false_positives = 0;
    for (uint64_t i = kKeys; i < kKeys + kProbes; ++i) {
      false_positives += reader.KeyMayMatch(shape.make(i)) ? 1 : 0;
    }
    const double rate = static_cast<double>(false_positives) / kProbes;
    EXPECT_LT(rate, 0.02) << shape.name;
    std::printf("[ bloom    ] %-20s 10 bits/key: false-positive rate %.4f\n", shape.name, rate);
  }
}

TEST(BloomTest, MoreBitsPerKeyMeansFewerFalsePositives) {
  constexpr uint64_t kKeys = 20000;
  std::vector<std::string> keys;
  for (uint64_t i = 0; i < kKeys; ++i) {
    keys.push_back(test::Key(i));
  }
  double previous = 1.0;
  for (const int bits : {2, 5, 10, 20}) {
    const std::string filter = BuildFilter(keys, bits);
    BloomFilterReader reader(filter);
    uint64_t false_positives = 0;
    for (uint64_t i = kKeys; i < 3 * kKeys; ++i) {
      false_positives += reader.KeyMayMatch(test::Key(i)) ? 1 : 0;
    }
    const double rate = static_cast<double>(false_positives) / (2 * kKeys);
    EXPECT_LT(rate, previous) << bits << " bits/key";
    previous = rate;
  }
  EXPECT_LT(previous, 0.001);  // 20 bits/key: theory about 0.007%.
}

}  // namespace
}  // namespace kvlog
