#include "util/crc32c.h"

#include <cstdint>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace kvlog::crc32c {
namespace {

// Test vectors from RFC 3720, section B.4.
TEST(Crc32cTest, StandardResults) {
  char buf[32];

  std::fill(buf, buf + sizeof(buf), 0);
  EXPECT_EQ(0x8a9136aau, Value(buf, sizeof(buf)));

  std::fill(buf, buf + sizeof(buf), static_cast<char>(0xff));
  EXPECT_EQ(0x62a8ab43u, Value(buf, sizeof(buf)));

  for (int i = 0; i < 32; ++i) {
    buf[i] = static_cast<char>(i);
  }
  EXPECT_EQ(0x46dd794eu, Value(buf, sizeof(buf)));

  for (int i = 0; i < 32; ++i) {
    buf[i] = static_cast<char>(31 - i);
  }
  EXPECT_EQ(0x113fdb5cu, Value(buf, sizeof(buf)));

  const unsigned char iscsi_read[48] = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  EXPECT_EQ(0xd9963a56u, Value(reinterpret_cast<const char*>(iscsi_read), sizeof(iscsi_read)));

  // The customary check value.
  EXPECT_EQ(0xe3069283u, Value("123456789", 9));
}

TEST(Crc32cTest, EmptyInput) {
  EXPECT_EQ(0u, Value("", 0));
  EXPECT_EQ(0x12345678u, Extend(0x12345678u, nullptr, 0));
}

TEST(Crc32cTest, DifferentInputsGiveDifferentValues) {
  EXPECT_NE(Value("a", 1), Value("foo", 3));
}

TEST(Crc32cTest, ExtendEqualsValueOfConcatenation) {
  EXPECT_EQ(Value("hello world", 11), Extend(Value("hello ", 6), "world", 5));
}

TEST(Crc32cTest, HardwareAndPortableImplementationsAgree) {
  std::mt19937_64 rng(301);
  for (size_t length = 0; length < 300; ++length) {
    std::string data(length, '\0');
    for (char& c : data) {
      c = static_cast<char>(rng());
    }
    // Different starting offsets exercise unaligned loads.
    for (size_t offset = 0; offset < std::min<size_t>(length, 9); ++offset) {
      const uint32_t init = static_cast<uint32_t>(rng());
      EXPECT_EQ(ExtendPortable(init, data.data() + offset, length - offset),
                Extend(init, data.data() + offset, length - offset));
    }
  }
  RecordProperty("hardware_accelerated", IsHardwareAccelerated() ? "yes" : "no");
}

TEST(Crc32cTest, MaskRoundTripsAndChangesValue) {
  const uint32_t crc = Value("foo", 3);
  EXPECT_NE(crc, Mask(crc));
  EXPECT_NE(crc, Mask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Mask(crc)));
  EXPECT_EQ(crc, Unmask(Unmask(Mask(Mask(crc)))));
}

}  // namespace
}  // namespace kvlog::crc32c
