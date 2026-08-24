#include "util/hash.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <string>

#include <gtest/gtest.h>

namespace kvlog {
namespace {

// Bytes (i * 131 + 7) mod 256 for i in [0, n): every byte value, no runs.
std::string Pattern(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) {
    s[i] = static_cast<char>((i * 131 + 7) % 256);
  }
  return s;
}

// Reference values of XXH64 with seed 0. The short ones are the values
// published with xxHash. All of them were also checked against an
// independent implementation: a zstd frame written with --check ends with
// the low 32 bits of XXH64(content, 0), which matched for these inputs and
// for Pattern(n), n in [0, 130].
TEST(HashTest, MatchesReferenceXxh64) {
  EXPECT_EQ(0xef46db3751d8e999ull, Hash64(""));
  EXPECT_EQ(0xd24ec4f1a98c6e5bull, Hash64("a"));
  EXPECT_EQ(0x44bc2cf5ad770999ull, Hash64("abc"));
  // 43 bytes: one 32-byte stripe, then an 8-byte, no 4-byte and 3 single
  // byte tail steps.
  EXPECT_EQ(0x0b242d361fda71bcull, Hash64("The quick brown fox jumps over the lazy dog"));
  // 67 bytes: two stripes and every kind of tail step.
  EXPECT_EQ(0x01ca5d1658e594a8ull,
            Hash64("0123456789012345678901234567890123456789abcdefghijklmnopqrstuvwxyz!"));
  EXPECT_EQ(0x9ddada11d3dc2d8full, Hash64(Pattern(100)));
}

// Filters on disk depend on these exact values: any change to the hash is a
// format change. This pins a spread of lengths across the stripe and tail
// boundaries via an order-sensitive digest.
TEST(HashTest, ValuesAreStable) {
  uint64_t digest = 0;
  for (size_t n = 0; n <= 300; ++n) {
    digest = Hash64(Pattern(n), digest);
  }
  const uint64_t first = digest;
  digest = 0;
  for (size_t n = 0; n <= 300; ++n) {
    digest = Hash64(Pattern(n), digest);
  }
  EXPECT_EQ(first, digest);
  // Recorded from this implementation after it matched the reference values
  // above; a change here means filters written earlier would be misread.
  EXPECT_EQ(0xd51e61c97f15de0aull, first);
}

TEST(HashTest, DoesNotDependOnAlignment) {
  const std::string data = Pattern(257);
  auto buffer = std::make_unique<char[]>(data.size() + 16);
  for (size_t shift = 0; shift < 16; ++shift) {
    std::memcpy(buffer.get() + shift, data.data(), data.size());
    for (const size_t n : {size_t{0}, size_t{3}, size_t{8}, size_t{31}, size_t{32}, size_t{257}}) {
      EXPECT_EQ(Hash64(data.data(), n, 0), Hash64(buffer.get() + shift, n, 0))
          << "shift " << shift << " length " << n;
    }
  }
}

TEST(HashTest, SeedAndEveryInputBitChangeTheResult) {
  EXPECT_NE(Hash64("key", 0), Hash64("key", 1));
  EXPECT_NE(Hash64("", 0), Hash64(std::string(1, '\0'), 0));  // Length counts.

  // Flipping any single bit of a 40-byte input gives a new value.
  const std::string base = Pattern(40);
  std::set<uint64_t> seen = {Hash64(base)};
  for (size_t bit = 0; bit < base.size() * 8; ++bit) {
    std::string flipped = base;
    flipped[bit / 8] = static_cast<char>(flipped[bit / 8] ^ (1 << (bit % 8)));
    EXPECT_TRUE(seen.insert(Hash64(flipped)).second) << "bit " << bit;
  }
}

TEST(HashTest, HighAndLowHalvesAreBothWellMixed) {
  // The Bloom filter uses both 32-bit halves; sequential keys must spread
  // over buckets in each half.
  constexpr int kBuckets = 64;
  constexpr int kKeys = 64000;
  int low[kBuckets] = {};
  int high[kBuckets] = {};
  for (int i = 0; i < kKeys; ++i) {
    const uint64_t h = Hash64("key" + std::to_string(i));
    ++low[h % kBuckets];
    ++high[(h >> 32) % kBuckets];
  }
  for (int b = 0; b < kBuckets; ++b) {
    // Expected 1000 per bucket; the standard deviation is about 31.
    EXPECT_NEAR(1000, low[b], 200) << b;
    EXPECT_NEAR(1000, high[b], 200) << b;
  }
}

}  // namespace
}  // namespace kvlog
