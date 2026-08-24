#include "util/coding.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace kvlog {
namespace {

TEST(CodingTest, Fixed32RoundTripsAndIsLittleEndian) {
  std::string s;
  for (uint32_t v = 0; v < 100000; ++v) {
    PutFixed32(&s, v);
  }
  const char* p = s.data();
  for (uint32_t v = 0; v < 100000; ++v) {
    EXPECT_EQ(v, DecodeFixed32(p));
    p += 4;
  }
  std::string one;
  PutFixed32(&one, 0x04030201u);
  EXPECT_EQ(std::string("\x01\x02\x03\x04", 4), one);
}

TEST(CodingTest, Fixed64RoundTripsAroundPowersOfTwo) {
  std::string s;
  for (int power = 0; power <= 63; ++power) {
    const uint64_t v = uint64_t{1} << power;
    PutFixed64(&s, v - 1);
    PutFixed64(&s, v);
    PutFixed64(&s, v + 1);
  }
  std::string_view input(s);
  for (int power = 0; power <= 63; ++power) {
    const uint64_t v = uint64_t{1} << power;
    uint64_t actual = 0;
    ASSERT_TRUE(GetFixed64(&input, &actual));
    EXPECT_EQ(v - 1, actual);
    ASSERT_TRUE(GetFixed64(&input, &actual));
    EXPECT_EQ(v, actual);
    ASSERT_TRUE(GetFixed64(&input, &actual));
    EXPECT_EQ(v + 1, actual);
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, Varint32RoundTrips) {
  std::string s;
  std::vector<uint32_t> values;
  for (uint32_t i = 0; i < 32 * 32; ++i) {
    values.push_back((i / 32) << (i % 32));
  }
  values.push_back(std::numeric_limits<uint32_t>::max());
  for (const uint32_t v : values) {
    PutVarint32(&s, v);
  }
  std::string_view input(s);
  for (const uint32_t expected : values) {
    const size_t before = input.size();
    uint32_t actual = 0;
    ASSERT_TRUE(GetVarint32(&input, &actual));
    EXPECT_EQ(expected, actual);
    EXPECT_EQ(static_cast<size_t>(VarintLength(actual)), before - input.size());
  }
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, Varint64RoundTrips) {
  std::vector<uint64_t> values = {0, 100, std::numeric_limits<uint64_t>::max(),
                                  std::numeric_limits<uint64_t>::max() - 1};
  for (int power = 0; power < 64; ++power) {
    const uint64_t v = uint64_t{1} << power;
    values.push_back(v);
    values.push_back(v - 1);
    values.push_back(v + 1);
  }
  std::string s;
  for (const uint64_t v : values) {
    PutVarint64(&s, v);
  }
  std::string_view input(s);
  for (const uint64_t expected : values) {
    uint64_t actual = 0;
    ASSERT_TRUE(GetVarint64(&input, &actual));
    EXPECT_EQ(expected, actual);
  }
  EXPECT_TRUE(input.empty());
  EXPECT_EQ(10, VarintLength(std::numeric_limits<uint64_t>::max()));
}

TEST(CodingTest, Varint32RejectsOverflow) {
  // Five bytes whose last byte carries more than the 4 bits a 32-bit value
  // has room for.
  const std::string encoded("\x81\x82\x83\x84\x85", 5);
  uint32_t value = 0;
  EXPECT_EQ(nullptr, GetVarint32Ptr(encoded.data(), encoded.data() + encoded.size(), &value));
  // Six continuation bytes are never valid for a 32-bit value.
  const std::string too_long("\xff\xff\xff\xff\xff\x01", 6);
  EXPECT_EQ(nullptr, GetVarint32Ptr(too_long.data(), too_long.data() + too_long.size(), &value));
}

TEST(CodingTest, Varint64RejectsOverflow) {
  const std::string encoded("\x81\x82\x83\x84\x85\x81\x82\x83\x84\x85\x11", 11);
  uint64_t value = 0;
  EXPECT_EQ(nullptr, GetVarint64Ptr(encoded.data(), encoded.data() + encoded.size(), &value));
  // A tenth byte above 1 would set bits past bit 63.
  const std::string ten("\xff\xff\xff\xff\xff\xff\xff\xff\xff\x02", 10);
  EXPECT_EQ(nullptr, GetVarint64Ptr(ten.data(), ten.data() + ten.size(), &value));
}

TEST(CodingTest, TruncatedVarintsAreRejected) {
  const uint64_t large = uint64_t{1} << 63;
  std::string s;
  PutVarint64(&s, large);
  uint64_t value = 0;
  for (size_t len = 0; len + 1 < s.size(); ++len) {
    EXPECT_EQ(nullptr, GetVarint64Ptr(s.data(), s.data() + len, &value)) << len;
  }
  EXPECT_NE(nullptr, GetVarint64Ptr(s.data(), s.data() + s.size(), &value));
  EXPECT_EQ(large, value);
}

TEST(CodingTest, LengthPrefixedRoundTrips) {
  std::string s;
  PutLengthPrefixed(&s, "");
  PutLengthPrefixed(&s, "foo");
  PutLengthPrefixed(&s, "bar");
  PutLengthPrefixed(&s, std::string(200, 'x'));
  std::string_view input(s);
  std::string_view v;
  ASSERT_TRUE(GetLengthPrefixed(&input, &v));
  EXPECT_EQ("", v);
  ASSERT_TRUE(GetLengthPrefixed(&input, &v));
  EXPECT_EQ("foo", v);
  ASSERT_TRUE(GetLengthPrefixed(&input, &v));
  EXPECT_EQ("bar", v);
  ASSERT_TRUE(GetLengthPrefixed(&input, &v));
  EXPECT_EQ(std::string(200, 'x'), v);
  EXPECT_TRUE(input.empty());
}

TEST(CodingTest, LengthPrefixedRejectsShortInput) {
  std::string s;
  PutLengthPrefixed(&s, "hello");
  s.pop_back();
  std::string_view input(s);
  std::string_view v;
  EXPECT_FALSE(GetLengthPrefixed(&input, &v));
}

TEST(CodingTest, FixedDecodersRejectShortInput) {
  std::string_view three("abc");
  uint32_t v32 = 0;
  EXPECT_FALSE(GetFixed32(&three, &v32));
  std::string_view seven("abcdefg");
  uint64_t v64 = 0;
  EXPECT_FALSE(GetFixed64(&seven, &v64));
}

}  // namespace
}  // namespace kvlog
