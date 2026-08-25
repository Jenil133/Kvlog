#include "db/skiplist.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "util/arena.h"

namespace kvlog {
namespace {

using Key = uint64_t;

struct KeyComparator {
  int operator()(const Key& a, const Key& b) const { return a < b ? -1 : (a > b ? 1 : 0); }
};

using TestList = SkipList<Key, KeyComparator>;

TEST(SkipListTest, Empty) {
  Arena arena;
  TestList list(KeyComparator(), &arena);
  EXPECT_FALSE(list.Contains(10));

  TestList::Iterator iter(&list);
  EXPECT_FALSE(iter.Valid());
  iter.SeekToFirst();
  EXPECT_FALSE(iter.Valid());
  iter.Seek(100);
  EXPECT_FALSE(iter.Valid());
}

TEST(SkipListTest, InsertLookupAndIterateMatchAModel) {
  constexpr int kInserts = 2000;
  constexpr Key kRange = 5000;
  std::mt19937_64 rng(1000);
  std::set<Key> model;
  Arena arena;
  TestList list(KeyComparator(), &arena);
  for (int i = 0; i < kInserts; ++i) {
    const Key key = rng() % kRange;
    if (model.insert(key).second) {
      list.Insert(key);
    }
  }

  for (Key k = 0; k < kRange; ++k) {
    EXPECT_EQ(model.count(k) == 1, list.Contains(k)) << k;
  }

  // A full scan yields exactly the model, in order.
  std::vector<Key> scanned;
  TestList::Iterator iter(&list);
  for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
    scanned.push_back(iter.key());
  }
  EXPECT_EQ(std::vector<Key>(model.begin(), model.end()), scanned);

  // Seek lands on the first key >= target, and Next continues in order.
  for (Key target = 0; target < kRange; ++target) {
    TestList::Iterator it(&list);
    it.Seek(target);
    auto expected = model.lower_bound(target);
    for (int step = 0; step < 3; ++step) {
      if (expected == model.end()) {
        ASSERT_FALSE(it.Valid());
        break;
      }
      ASSERT_TRUE(it.Valid());
      ASSERT_EQ(*expected, it.key());
      ++expected;
      it.Next();
    }
  }
}

// One writer inserts while several readers scan continuously, with no locks.
// Readers check that every scan is sorted, contains only inserted keys, and
// never shrinks: once a key is visible it stays visible. After the writer
// finishes, every reader must see every key.
TEST(SkipListTest, ConcurrentReadersSeeConsistentSortedPrefixes) {
  constexpr Key kKeys = 20000;
  constexpr int kReaders = 3;
  std::vector<Key> order(kKeys);
  std::iota(order.begin(), order.end(), Key{1});
  std::shuffle(order.begin(), order.end(), std::mt19937_64(7));

  Arena arena;
  TestList list(KeyComparator(), &arena);
  std::atomic<bool> writer_done{false};
  std::atomic<int> failures{0};

  auto reader = [&] {
    size_t previous_count = 0;
    while (true) {
      // Read the flag before scanning: if it was already set, this scan
      // happens after all inserts and must see all of them.
      const bool final_scan = writer_done.load(std::memory_order_acquire);
      size_t count = 0;
      Key previous = 0;
      TestList::Iterator iter(&list);
      for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
        const Key key = iter.key();
        if (key == 0 || key > kKeys || key <= previous) {
          failures.fetch_add(1);
        }
        previous = key;
        ++count;
      }
      if (count < previous_count) {
        failures.fetch_add(1);
      }
      previous_count = count;
      if (final_scan) {
        if (count != kKeys) {
          failures.fetch_add(1);
        }
        return;
      }
    }
  };

  std::vector<std::thread> readers;
  for (int i = 0; i < kReaders; ++i) {
    readers.emplace_back(reader);
  }
  for (const Key key : order) {
    list.Insert(key);
  }
  writer_done.store(true, std::memory_order_release);
  for (auto& t : readers) {
    t.join();
  }
  EXPECT_EQ(0, failures.load());
}

}  // namespace
}  // namespace kvlog
