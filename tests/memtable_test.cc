#include "db/memtable.h"

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "test_util.h"

namespace kvlog {
namespace {

class MemTableTest : public testing::Test {
 protected:
  // Result of Get at `sequence`: the value, "DELETED", or "ABSENT".
  std::string Lookup(std::string_view key, SequenceNumber sequence) {
    std::string value;
    Status s;
    if (!mem_.Get(LookupKey(key, sequence), &value, &s)) {
      return "ABSENT";
    }
    if (s.IsNotFound()) {
      return "DELETED";
    }
    EXPECT_TRUE(s.ok()) << s.ToString();
    return value;
  }

  const InternalKeyComparator icmp_{BytewiseComparator()};
  MemTable mem_{icmp_};
};

TEST_F(MemTableTest, EmptyTable) {
  EXPECT_TRUE(mem_.Empty());
  EXPECT_EQ("ABSENT", Lookup("anything", kMaxSequenceNumber));
  auto iter = mem_.NewIterator();
  iter->SeekToFirst();
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
}

TEST_F(MemTableTest, GetReturnsNewestVersionVisibleAtTheSequence) {
  mem_.Add(1, kTypeValue, "k", "v1");
  mem_.Add(5, kTypeValue, "k", "v5");
  mem_.Add(9, kTypeDeletion, "k", "");
  mem_.Add(12, kTypeValue, "k", "v12");
  mem_.Add(3, kTypeValue, "other", "o3");
  EXPECT_FALSE(mem_.Empty());
  EXPECT_EQ(5u, mem_.NumEntries());

  EXPECT_EQ("ABSENT", Lookup("k", 0));
  EXPECT_EQ("v1", Lookup("k", 1));
  EXPECT_EQ("v1", Lookup("k", 4));
  EXPECT_EQ("v5", Lookup("k", 5));
  EXPECT_EQ("v5", Lookup("k", 8));
  EXPECT_EQ("DELETED", Lookup("k", 9));   // A tombstone answers the lookup...
  EXPECT_EQ("DELETED", Lookup("k", 11));  // ...until a newer value appears.
  EXPECT_EQ("v12", Lookup("k", 12));
  EXPECT_EQ("v12", Lookup("k", kMaxSequenceNumber));

  EXPECT_EQ("ABSENT", Lookup("other", 2));
  EXPECT_EQ("o3", Lookup("other", 3));
  // Keys that are prefixes or extensions of stored keys do not match.
  EXPECT_EQ("ABSENT", Lookup("", 100));
  EXPECT_EQ("ABSENT", Lookup("kk", 100));
  EXPECT_EQ("ABSENT", Lookup("othe", 100));
}

TEST_F(MemTableTest, EmptyKeysValuesAndBinaryData) {
  const std::string binary_key("\x00\xff\x00", 3);
  const std::string binary_value("\xff\x00\x7f\x80", 4);
  mem_.Add(1, kTypeValue, "", "empty-key");
  mem_.Add(2, kTypeValue, "empty-value", "");
  mem_.Add(3, kTypeValue, binary_key, binary_value);
  EXPECT_EQ("empty-key", Lookup("", 10));
  EXPECT_EQ("", Lookup("empty-value", 10));
  EXPECT_EQ(binary_value, Lookup(binary_key, 10));
  EXPECT_EQ("ABSENT", Lookup(std::string("\x00\xff", 2), 10));
}

TEST_F(MemTableTest, IteratorYieldsInternalKeyOrder) {
  std::mt19937_64 rng(5);
  std::vector<std::pair<std::string, std::string>> expected;  // (internal key, value)
  SequenceNumber seq = 1;
  for (int i = 0; i < 500; ++i) {
    const std::string key = test::Key(rng() % 100);
    const bool deletion = rng() % 4 == 0;
    const std::string value = deletion ? "" : test::RandomBytes(&rng, rng() % 20);
    mem_.Add(seq, deletion ? kTypeDeletion : kTypeValue, key, value);
    std::string ikey;
    AppendInternalKey(&ikey, ParsedInternalKey(key, seq, deletion ? kTypeDeletion : kTypeValue));
    expected.emplace_back(ikey, value);
    ++seq;
  }
  std::sort(expected.begin(), expected.end(), [&](const auto& a, const auto& b) {
    return icmp_.Compare(a.first, b.first) < 0;
  });

  auto iter = mem_.NewIterator();
  size_t i = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next(), ++i) {
    ASSERT_LT(i, expected.size());
    EXPECT_EQ(expected[i].first, iter->key());
    EXPECT_EQ(expected[i].second, iter->value());
  }
  EXPECT_EQ(expected.size(), i);

  // Seek to an internal key lands on the first entry at or after it.
  for (size_t j = 0; j < expected.size(); j += 7) {
    iter->Seek(expected[j].first);
    ASSERT_TRUE(iter->Valid());
    EXPECT_EQ(expected[j].first, iter->key());
  }
}

TEST_F(MemTableTest, MemoryUsageGrowsWithEntries) {
  const size_t initial = mem_.ApproximateMemoryUsage();
  for (int i = 0; i < 1000; ++i) {
    mem_.Add(static_cast<SequenceNumber>(i + 1), kTypeValue, test::Key(i), std::string(100, 'v'));
  }
  EXPECT_GE(mem_.ApproximateMemoryUsage(), initial + 1000 * 100);
}

// An empty memtable already owns an arena block (the skiplist head lives in
// it). The write buffer limit applies to what was added since, so a buffer
// of one block is not full before its first entry.
TEST_F(MemTableTest, BytesAddedStartsAtZero) {
  EXPECT_GT(mem_.ApproximateMemoryUsage(), 0u);
  EXPECT_EQ(0u, mem_.BytesAdded());
  for (int i = 0; i < 1000; ++i) {
    mem_.Add(static_cast<SequenceNumber>(i + 1), kTypeValue, test::Key(i), std::string(100, 'v'));
  }
  EXPECT_EQ(mem_.ApproximateMemoryUsage() - mem_.BytesAdded(),
            MemTable(icmp_).ApproximateMemoryUsage());
}

}  // namespace
}  // namespace kvlog
