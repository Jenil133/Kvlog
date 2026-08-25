#include "table/filter_block.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "test_util.h"

namespace kvlog {
namespace {

TEST(FilterBlockTest, RoundTripsKeys) {
  FilterBlockBuilder builder(10);
  for (uint64_t i = 0; i < 5000; i += 2) {
    builder.AddKey(test::Key(i));
  }
  EXPECT_EQ(2500u, builder.NumKeys());
  const std::string block = builder.Finish();
  FilterBlockReader reader(block);
  int false_positives = 0;
  for (uint64_t i = 0; i < 5000; ++i) {
    if (i % 2 == 0) {
      ASSERT_TRUE(reader.KeyMayMatch(test::Key(i))) << i;
    } else {
      false_positives += reader.KeyMayMatch(test::Key(i)) ? 1 : 0;
    }
  }
  EXPECT_LT(false_positives, 50);  // 2% of 2500.
}

TEST(FilterBlockTest, RepeatedUserKeysAreAddedOnce) {
  FilterBlockBuilder builder(10);
  // Versions of one user key are adjacent in a table.
  for (const char* key : {"a", "a", "a", "b", "c", "c", "a"}) {
    builder.AddKey(key);
  }
  // "a" again after "c" cannot happen in a table; it is just added again.
  EXPECT_EQ(4u, builder.NumKeys());
  const std::string block = builder.Finish();
  FilterBlockReader reader(block);
  for (const char* key : {"a", "b", "c"}) {
    EXPECT_TRUE(reader.KeyMayMatch(key)) << key;
  }
}

TEST(FilterBlockTest, EmptyAndBinaryKeys) {
  FilterBlockBuilder builder(10);
  builder.AddKey("");
  builder.AddKey(std::string("\0\0", 2));
  builder.AddKey(std::string("\xff\x00\xff", 3));
  EXPECT_EQ(3u, builder.NumKeys());
  const std::string block = builder.Finish();
  FilterBlockReader reader(block);
  EXPECT_TRUE(reader.KeyMayMatch(""));
  EXPECT_TRUE(reader.KeyMayMatch(std::string("\0\0", 2)));
  EXPECT_TRUE(reader.KeyMayMatch(std::string("\xff\x00\xff", 3)));
}

TEST(FilterBlockTest, BuilderWithNoKeysGivesAFilterThatRejectsEverything) {
  FilterBlockBuilder builder(10);
  const std::string block = builder.Finish();
  EXPECT_FALSE(block.empty());
  FilterBlockReader reader(block);
  EXPECT_FALSE(reader.KeyMayMatch("x"));
  EXPECT_FALSE(reader.KeyMayMatch(""));
}

}  // namespace
}  // namespace kvlog
