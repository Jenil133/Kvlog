#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_iter.h"
#include "db/dbformat.h"
#include "table/iterator_util.h"
#include "table/merging_iterator.h"
#include "table/two_level_iterator.h"
#include "test_util.h"
#include "util/comparator.h"

namespace kvlog {
namespace {

using Entry = std::pair<std::string, std::string>;

// Iterates over a vector of entries that is already sorted by `comparator`.
// With an error status, the iterator ends early at `fail_at` and reports it.
class VectorIterator final : public Iterator {
 public:
  VectorIterator(std::vector<Entry> entries, const Comparator* comparator,
                 Status error = Status::OK(), size_t fail_at = SIZE_MAX)
      : entries_(std::move(entries)),
        comparator_(comparator),
        error_(std::move(error)),
        fail_at_(fail_at),
        index_(entries_.size()) {}

  bool Valid() const override { return index_ < Limit(); }
  void SeekToFirst() override { index_ = 0; }
  void Seek(std::string_view target) override {
    index_ = 0;
    while (index_ < Limit() && comparator_->Compare(entries_[index_].first, target) < 0) {
      ++index_;
    }
  }
  void Next() override { ++index_; }
  std::string_view key() const override { return entries_[index_].first; }
  std::string_view value() const override { return entries_[index_].second; }
  Status status() const override {
    return index_ >= std::min(fail_at_, entries_.size()) ? error_ : Status::OK();
  }

 private:
  size_t Limit() const {
    return error_.ok() ? entries_.size() : std::min(fail_at_, entries_.size());
  }

  const std::vector<Entry> entries_;
  const Comparator* const comparator_;
  const Status error_;
  const size_t fail_at_;
  size_t index_;
};

std::vector<Entry> Drain(Iterator* iter) {
  std::vector<Entry> result;
  for (; iter->Valid(); iter->Next()) {
    result.emplace_back(std::string(iter->key()), std::string(iter->value()));
  }
  return result;
}

// ---------------------------------------------------------------------------
// MergingIterator

TEST(MergingIteratorTest, MergesSortedChildrenAndPrefersEarlierChildrenOnTies) {
  const Comparator* const cmp = BytewiseComparator();
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<Entry>{{"b", "newest-b"}, {"d", "d"}}, cmp));
  children.push_back(std::make_unique<VectorIterator>(std::vector<Entry>{}, cmp));
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<Entry>{{"a", "a"}, {"b", "older-b"}, {"e", "e"}}, cmp));
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<Entry>{{"b", "oldest-b"}, {"c", "c"}}, cmp));
  auto merged = NewMergingIterator(cmp, std::move(children));

  merged->SeekToFirst();
  EXPECT_EQ((std::vector<Entry>{{"a", "a"},
                                {"b", "newest-b"},
                                {"b", "older-b"},
                                {"b", "oldest-b"},
                                {"c", "c"},
                                {"d", "d"},
                                {"e", "e"}}),
            Drain(merged.get()));
  EXPECT_TRUE(merged->status().ok());

  merged->Seek("bb");
  EXPECT_EQ((std::vector<Entry>{{"c", "c"}, {"d", "d"}, {"e", "e"}}), Drain(merged.get()));
  merged->Seek("f");
  EXPECT_FALSE(merged->Valid());
}

TEST(MergingIteratorTest, MatchesASortedUnionOfRandomChildren) {
  const Comparator* const cmp = BytewiseComparator();
  std::mt19937_64 rng(42);
  std::vector<std::unique_ptr<Iterator>> children;
  std::vector<Entry> all;
  for (int child = 0; child < 9; ++child) {
    std::map<std::string, std::string> entries;
    const int count = static_cast<int>(rng() % 200);
    for (int i = 0; i < count; ++i) {
      // The child number in the value keeps entries distinct, and makes the
      // tie order (lower child first) checkable through stable_sort below.
      entries.emplace(test::RandomBytes(&rng, 1 + rng() % 3), "child" + std::to_string(child));
    }
    std::vector<Entry> sorted(entries.begin(), entries.end());
    all.insert(all.end(), sorted.begin(), sorted.end());
    children.push_back(std::make_unique<VectorIterator>(std::move(sorted), cmp));
  }
  std::stable_sort(all.begin(), all.end(),
                   [](const Entry& a, const Entry& b) { return a.first < b.first; });

  auto merged = NewMergingIterator(cmp, std::move(children));
  merged->SeekToFirst();
  EXPECT_EQ(all, Drain(merged.get()));
}

TEST(MergingIteratorTest, StopsAtTheFirstChildError) {
  const Comparator* const cmp = BytewiseComparator();
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<Entry>{{"a", "1"}, {"c", "3"}, {"e", "5"}}, cmp));
  children.push_back(std::make_unique<VectorIterator>(
      std::vector<Entry>{{"b", "2"}, {"d", "4"}}, cmp, Status::Corruption("bad block"), 1));
  auto merged = NewMergingIterator(cmp, std::move(children));
  merged->SeekToFirst();
  const std::vector<Entry> seen = Drain(merged.get());
  EXPECT_TRUE(merged->status().IsCorruption());
  // Nothing after the failing child's last good entry is returned, so the
  // caller never sees a result with a silent hole in it.
  EXPECT_EQ((std::vector<Entry>{{"a", "1"}, {"b", "2"}}), seen);
}

TEST(MergingIteratorTest, NoChildrenAndOneChild) {
  const Comparator* const cmp = BytewiseComparator();
  auto empty = NewMergingIterator(cmp, {});
  empty->SeekToFirst();
  EXPECT_FALSE(empty->Valid());
  EXPECT_TRUE(empty->status().ok());

  std::vector<std::unique_ptr<Iterator>> one;
  one.push_back(std::make_unique<VectorIterator>(std::vector<Entry>{{"k", "v"}}, cmp));
  auto single = NewMergingIterator(cmp, std::move(one));
  single->SeekToFirst();
  EXPECT_EQ((std::vector<Entry>{{"k", "v"}}), Drain(single.get()));
}

// ---------------------------------------------------------------------------
// TwoLevelIterator

class TwoLevelIteratorTest : public testing::Test {
 protected:
  // Blocks are named by their index value ("0", "1", ...). The index key of
  // each block is its last key; empty blocks reuse the previous block's.
  std::unique_ptr<Iterator> Make(const std::vector<std::vector<Entry>>& blocks,
                                 int failing_block = -1) {
    std::vector<Entry> index;
    std::string last_key;
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (!blocks[i].empty()) {
        last_key = blocks[i].back().first;
      }
      index.emplace_back(last_key, std::to_string(i));
    }
    const Comparator* const cmp = BytewiseComparator();
    return NewTwoLevelIterator(
        std::make_unique<VectorIterator>(std::move(index), cmp),
        [this, blocks, failing_block, cmp](std::string_view handle) -> std::unique_ptr<Iterator> {
          ++opens_;
          const int block = std::stoi(std::string(handle));
          if (block == failing_block) {
            return NewErrorIterator(Status::IOError("cannot read block"));
          }
          return std::make_unique<VectorIterator>(blocks[static_cast<size_t>(block)], cmp);
        });
  }

  int opens_ = 0;
};

TEST_F(TwoLevelIteratorTest, WalksAllBlocksSkippingEmptyOnes) {
  auto iter = Make({{{"a", "1"}, {"b", "2"}}, {}, {{"c", "3"}}, {}, {}, {{"d", "4"}, {"e", "5"}}});
  iter->SeekToFirst();
  EXPECT_EQ((std::vector<Entry>{{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"}}),
            Drain(iter.get()));
  EXPECT_TRUE(iter->status().ok());

  iter->Seek("bb");
  EXPECT_EQ((std::vector<Entry>{{"c", "3"}, {"d", "4"}, {"e", "5"}}), Drain(iter.get()));
  iter->Seek("d");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("d", iter->key());
  iter->Seek("z");
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
}

TEST_F(TwoLevelIteratorTest, ReusesTheOpenBlockForSeeksWithinIt) {
  auto iter = Make({{{"a", "1"}, {"b", "2"}, {"c", "3"}}, {{"d", "4"}}});
  iter->Seek("a");
  iter->Seek("b");
  iter->Seek("c");
  EXPECT_EQ(1, opens_);
}

TEST_F(TwoLevelIteratorTest, StopsAtABlockThatFailsToOpen) {
  auto iter = Make({{{"a", "1"}}, {{"b", "2"}}, {{"c", "3"}}}, /*failing_block=*/1);
  iter->SeekToFirst();
  EXPECT_EQ((std::vector<Entry>{{"a", "1"}}), Drain(iter.get()));
  EXPECT_TRUE(iter->status().IsIOError());
}

// ---------------------------------------------------------------------------
// DBIter: user-visible view of an internal iterator

class DBIterTest : public testing::Test {
 protected:
  void Add(std::string_view user_key, SequenceNumber sequence, ValueType type,
           std::string_view value = "") {
    std::string key;
    AppendInternalKey(&key, ParsedInternalKey(user_key, sequence, type));
    entries_.emplace_back(key, std::string(value));
  }

  std::unique_ptr<Iterator> Open(SequenceNumber snapshot) {
    std::sort(entries_.begin(), entries_.end(), [this](const Entry& a, const Entry& b) {
      return icmp_.Compare(a.first, b.first) < 0;
    });
    return NewDBIterator(std::make_unique<VectorIterator>(entries_, &icmp_), BytewiseComparator(),
                         snapshot);
  }

  static std::vector<Entry> Scan(Iterator* iter) {
    iter->SeekToFirst();
    return Drain(iter);
  }

  const InternalKeyComparator icmp_{BytewiseComparator()};
  std::vector<Entry> entries_;
};

TEST_F(DBIterTest, YieldsTheNewestVisibleVersionOfEachKey) {
  Add("a", 1, kTypeValue, "a1");
  Add("a", 5, kTypeValue, "a5");
  Add("b", 2, kTypeValue, "b2");
  Add("b", 6, kTypeDeletion);
  Add("c", 3, kTypeDeletion);
  Add("c", 7, kTypeValue, "c7");
  Add("d", 4, kTypeValue, "d4");

  EXPECT_EQ((std::vector<Entry>{{"a", "a5"}, {"c", "c7"}, {"d", "d4"}}),
            Scan(Open(kMaxSequenceNumber).get()));
  // Snapshot at 5: b's tombstone (6) and c's value (7) are in the future.
  EXPECT_EQ((std::vector<Entry>{{"a", "a5"}, {"b", "b2"}, {"d", "d4"}}), Scan(Open(5).get()));
  EXPECT_EQ((std::vector<Entry>{{"a", "a1"}, {"b", "b2"}}), Scan(Open(2).get()));
  EXPECT_EQ((std::vector<Entry>{}), Scan(Open(0).get()));
}

TEST_F(DBIterTest, SeekSkipsDeletedAndFutureVersions) {
  Add("a", 1, kTypeValue, "a1");
  Add("b", 2, kTypeValue, "b2");
  Add("b", 3, kTypeDeletion);
  Add("c", 4, kTypeValue, "c4");
  Add("c", 9, kTypeValue, "c9");
  auto iter = Open(5);
  iter->Seek("b");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("c", iter->key());
  EXPECT_EQ("c4", iter->value());
  iter->Next();
  EXPECT_FALSE(iter->Valid());

  iter->Seek("");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ("a", iter->key());
  iter->Seek("c\x01");
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().ok());
}

TEST_F(DBIterTest, EmptyUserKeyAndEmptyValues) {
  Add("", 1, kTypeValue, "");
  Add("x", 2, kTypeValue, "");
  EXPECT_EQ((std::vector<Entry>{{"", ""}, {"x", ""}}), Scan(Open(kMaxSequenceNumber).get()));
}

TEST_F(DBIterTest, MalformedInternalKeyIsCorruption) {
  Add("a", 1, kTypeValue, "a1");
  entries_.emplace_back(std::string("short"), "v");  // Shorter than a tag.
  auto iter = NewDBIterator(std::make_unique<VectorIterator>(entries_, &icmp_),
                            BytewiseComparator(), kMaxSequenceNumber);
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  iter->Next();
  EXPECT_FALSE(iter->Valid());
  EXPECT_TRUE(iter->status().IsCorruption());
}

TEST_F(DBIterTest, MatchesAModelOverRandomHistories) {
  std::mt19937_64 rng(77);
  // model[snapshot] = visible state after the write with that sequence.
  std::map<std::string, std::string> state;
  std::vector<std::map<std::string, std::string>> history{state};
  for (SequenceNumber seq = 1; seq <= 600; ++seq) {
    const std::string key = test::Key(rng() % 40);
    if (rng() % 3 == 0) {
      Add(key, seq, kTypeDeletion);
      state.erase(key);
    } else {
      const std::string value = "v" + std::to_string(seq);
      Add(key, seq, kTypeValue, value);
      state[key] = value;
    }
    history.push_back(state);
  }
  for (SequenceNumber snapshot = 0; snapshot <= 600; snapshot += 37) {
    const auto& expected = history[snapshot];
    EXPECT_EQ(std::vector<Entry>(expected.begin(), expected.end()), Scan(Open(snapshot).get()))
        << "snapshot " << snapshot;
  }
}

}  // namespace
}  // namespace kvlog
