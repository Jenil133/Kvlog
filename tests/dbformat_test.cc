#include "db/dbformat.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "util/coding.h"

namespace kvlog {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq, ValueType type) {
  std::string encoded;
  AppendInternalKey(&encoded, ParsedInternalKey(user_key, seq, type));
  return encoded;
}

TEST(FormatTest, InternalKeysRoundTrip) {
  const std::vector<std::string> keys = {"", "k", "hello", "longggggggggggggggggggggg",
                                         std::string("\0\xff\x01", 3)};
  const SequenceNumber seqs[] = {1,       2,       3,       (1ull << 8) - 1,  1ull << 8,
                                 (1ull << 8) + 1,  (1ull << 16) - 1, 1ull << 16, (1ull << 16) + 1,
                                 (1ull << 32) - 1, 1ull << 32,       (1ull << 32) + 1,
                                 kMaxSequenceNumber};
  for (const std::string& key : keys) {
    for (const SequenceNumber seq : seqs) {
      for (const ValueType type : {kTypeValue, kTypeDeletion}) {
        const std::string encoded = IKey(key, seq, type);
        EXPECT_EQ(key.size() + 8, encoded.size());
        ParsedInternalKey decoded;
        ASSERT_TRUE(ParseInternalKey(encoded, &decoded));
        EXPECT_EQ(key, decoded.user_key);
        EXPECT_EQ(seq, decoded.sequence);
        EXPECT_EQ(type, decoded.type);
        EXPECT_EQ(key, ExtractUserKey(encoded));
      }
    }
  }
}

TEST(FormatTest, ParseRejectsMalformedKeys) {
  ParsedInternalKey decoded;
  EXPECT_FALSE(ParseInternalKey("", &decoded));
  EXPECT_FALSE(ParseInternalKey("1234567", &decoded));  // Shorter than a tag.
  std::string bad_type = "key";
  PutFixed64(&bad_type, (uint64_t{100} << 8) | 0x7);
  EXPECT_FALSE(ParseInternalKey(bad_type, &decoded));
}

TEST(FormatTest, InternalKeyOrder) {
  const InternalKeyComparator icmp(BytewiseComparator());
  // User keys ascending.
  EXPECT_LT(icmp.Compare(IKey("a", 1, kTypeValue), IKey("b", 100, kTypeValue)), 0);
  EXPECT_LT(icmp.Compare(IKey("", 1, kTypeValue), IKey("a", 1, kTypeValue)), 0);
  EXPECT_LT(icmp.Compare(IKey("a", 1, kTypeValue), IKey("ab", 1, kTypeValue)), 0);
  // Bytes compare unsigned: 0xff sorts after 'z'.
  EXPECT_LT(icmp.Compare(IKey("z", 1, kTypeValue), IKey("\xff", 1, kTypeValue)), 0);
  // Same user key: newer (higher) sequence first.
  EXPECT_LT(icmp.Compare(IKey("a", 100, kTypeValue), IKey("a", 99, kTypeValue)), 0);
  // Same user key and sequence: higher type first.
  EXPECT_LT(icmp.Compare(IKey("a", 5, kTypeValue), IKey("a", 5, kTypeDeletion)), 0);
  EXPECT_EQ(0, icmp.Compare(IKey("a", 5, kTypeValue), IKey("a", 5, kTypeValue)));
  // A seek key at sequence S sorts before every version <= S and after newer ones.
  const std::string seek = IKey("a", 50, kValueTypeForSeek);
  EXPECT_LT(icmp.Compare(IKey("a", 51, kTypeValue), seek), 0);
  EXPECT_LE(icmp.Compare(seek, IKey("a", 50, kTypeValue)), 0);
  EXPECT_LT(icmp.Compare(seek, IKey("a", 50, kTypeDeletion)), 0);
  EXPECT_LT(icmp.Compare(seek, IKey("a", 49, kTypeValue)), 0);
}

TEST(FormatTest, InternalKeyClassAndDebugString) {
  const InternalKey key("user\x01", 42, kTypeDeletion);
  EXPECT_EQ("user\x01", key.user_key());
  EXPECT_EQ("user\\x01 @ 42 : deletion", key.DebugString());
  InternalKey copy;
  EXPECT_TRUE(copy.empty());
  ASSERT_TRUE(copy.DecodeFrom(key.Encode()));
  EXPECT_EQ(key.Encode(), copy.Encode());
}

TEST(FormatTest, LookupKeyLayout) {
  for (const size_t length : {size_t{0}, size_t{5}, size_t{180}, size_t{190}, size_t{5000}}) {
    const std::string user_key(length, 'q');
    const LookupKey lookup(user_key, 77);
    EXPECT_EQ(user_key, lookup.user_key());
    EXPECT_EQ(IKey(user_key, 77, kValueTypeForSeek), lookup.internal_key());
    // The memtable key is the internal key with a varint32 length prefix.
    std::string expected_memtable_key;
    PutVarint32(&expected_memtable_key, static_cast<uint32_t>(length + 8));
    expected_memtable_key += lookup.internal_key();
    EXPECT_EQ(expected_memtable_key, lookup.memtable_key());
  }
}

}  // namespace
}  // namespace kvlog
