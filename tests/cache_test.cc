#include "util/cache.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db_test_util.h"
#include "kvlog/statistics.h"
#include "test_util.h"

namespace kvlog {
namespace {

// Values are heap-allocated copies of their key plus a payload, so a test
// can check that a handle's value belongs to the key it was looked up by.
struct TestValue {
  std::string key;
  int payload;
};

// Deleters are plain function pointers, so the deletion log is global.
// Tests reset it at the start; the concurrent test only counts.
std::atomic<int> g_deletions{0};
std::vector<std::pair<std::string, int>>* g_deleted = nullptr;  // Optional log.

void DeleteTestValue(std::string_view key, void* value) {
  auto* v = static_cast<TestValue*>(value);
  EXPECT_EQ(v->key, key);
  if (g_deleted != nullptr) {
    g_deleted->emplace_back(std::string(key), v->payload);
  }
  g_deletions.fetch_add(1, std::memory_order_relaxed);
  delete v;
}

class CacheTest : public testing::Test {
 protected:
  CacheTest() {
    g_deletions = 0;
    g_deleted = &deleted_;
  }
  ~CacheTest() override { g_deleted = nullptr; }

  // One shard, so eviction order is exactly LRU over every key.
  void MakeCache(size_t capacity, int shard_bits = 0) {
    cache_ = std::make_unique<Cache>(capacity, shard_bits);
  }

  Cache::Handle* Insert(const std::string& key, int payload, size_t charge = 1) {
    return cache_->Insert(key, new TestValue{key, payload}, charge, &DeleteTestValue);
  }
  // Inserts and immediately releases.
  void Put(const std::string& key, int payload, size_t charge = 1) {
    cache_->Release(Insert(key, payload, charge));
  }
  // Payload stored under `key`, or -1 if absent.
  int Get(const std::string& key) {
    Cache::Handle* h = cache_->Lookup(key);
    if (h == nullptr) {
      return -1;
    }
    const int payload = static_cast<TestValue*>(cache_->Value(h))->payload;
    cache_->Release(h);
    return payload;
  }

  std::vector<std::pair<std::string, int>> deleted_;
  std::unique_ptr<Cache> cache_;
};

TEST_F(CacheTest, HitsAndMisses) {
  MakeCache(100);
  EXPECT_EQ(-1, Get("a"));
  Put("a", 1);
  Put("b", 2);
  EXPECT_EQ(1, Get("a"));
  EXPECT_EQ(2, Get("b"));
  EXPECT_EQ(-1, Get("c"));
  const Cache::Stats stats = cache_->GetStats();
  EXPECT_EQ(2u, stats.hits);
  EXPECT_EQ(2u, stats.misses);
  EXPECT_EQ(2u, stats.inserts);
  EXPECT_EQ(0u, stats.evictions);
  EXPECT_EQ(2u, cache_->TotalCharge());
  EXPECT_EQ(0, g_deletions.load());
}

TEST_F(CacheTest, EvictsLeastRecentlyUsedFirst) {
  MakeCache(3);
  Put("a", 1);
  Put("b", 2);
  Put("c", 3);
  EXPECT_EQ(1, Get("a"));  // Order now b, c, a.
  Put("d", 4);             // Evicts b.
  EXPECT_EQ(-1, Get("b"));
  EXPECT_EQ(1, Get("a"));  // Order c, d, a.
  Put("e", 5);             // Evicts c.
  EXPECT_EQ(-1, Get("c"));
  EXPECT_EQ(4, Get("d"));
  EXPECT_EQ(5, Get("e"));
  EXPECT_EQ(1, Get("a"));
  const std::vector<std::pair<std::string, int>> expected = {{"b", 2}, {"c", 3}};
  EXPECT_EQ(expected, deleted_);
  EXPECT_EQ(2u, cache_->GetStats().evictions);
}

TEST_F(CacheTest, ChargesAreEnforcedInBytes) {
  MakeCache(1000);
  for (int i = 0; i < 100; ++i) {
    Put("k" + std::to_string(i), i, 70);
    EXPECT_LE(cache_->TotalCharge(), 1000u);
  }
  // 14 entries of 70 fit in 1000.
  EXPECT_EQ(980u, cache_->TotalCharge());
  EXPECT_EQ(86, g_deletions.load());
  for (int i = 86; i < 100; ++i) {
    EXPECT_EQ(i, Get("k" + std::to_string(i)));
  }

  // One large entry pushes out several small ones.
  Put("big", 1000, 500);
  EXPECT_LE(cache_->TotalCharge(), 1000u);
  EXPECT_EQ(1000, Get("big"));
  EXPECT_EQ(-1, Get("k86"));
}

TEST_F(CacheTest, EntryLargerThanTheCacheIsUsableWhilePinned) {
  MakeCache(10);
  Put("small", 1, 5);
  Cache::Handle* h = Insert("huge", 2, 50);
  EXPECT_EQ(2, static_cast<TestValue*>(cache_->Value(h))->payload);
  EXPECT_EQ(-1, Get("small"));  // Evicted to make what room there is.
  cache_->Release(h);
  // Once unpinned it cannot stay: it alone exceeds the capacity.
  EXPECT_EQ(-1, Get("huge"));
  EXPECT_EQ(0u, cache_->TotalCharge());
  EXPECT_EQ(2, g_deletions.load());
}

TEST_F(CacheTest, PinnedEntriesSurviveEviction) {
  MakeCache(3);
  Cache::Handle* pinned = Insert("pinned", 42);
  for (int i = 0; i < 20; ++i) {
    Put("k" + std::to_string(i), i);
  }
  // Every other entry came and went; the pinned one is still there.
  EXPECT_EQ(42, static_cast<TestValue*>(cache_->Value(pinned))->payload);
  EXPECT_EQ(42, Get("pinned"));
  EXPECT_LE(cache_->TotalCharge(), 3u);
  cache_->Release(pinned);
  EXPECT_EQ(42, Get("pinned"));  // Unpinned now, but recently used.
  Put("x", 1);
  Put("y", 2);
  Put("z", 3);
  EXPECT_EQ(-1, Get("pinned"));
}

TEST_F(CacheTest, PinnedEntriesMayExceedTheCapacityUntilReleased) {
  MakeCache(3);
  std::vector<Cache::Handle*> handles;
  for (int i = 0; i < 6; ++i) {
    handles.push_back(Insert("k" + std::to_string(i), i));
  }
  EXPECT_EQ(6u, cache_->TotalCharge());
  EXPECT_EQ(0, g_deletions.load());
  for (Cache::Handle* h : handles) {
    cache_->Release(h);
  }
  EXPECT_EQ(3u, cache_->TotalCharge());
  EXPECT_EQ(3, g_deletions.load());
}

TEST_F(CacheTest, EraseRemovesAndFreesOnceUnpinned) {
  MakeCache(100);
  Put("a", 1);
  cache_->Erase("a");
  EXPECT_EQ(-1, Get("a"));
  EXPECT_EQ(1, g_deletions.load());
  EXPECT_EQ(0u, cache_->TotalCharge());

  Cache::Handle* h = Insert("b", 2);
  cache_->Erase("b");
  EXPECT_EQ(-1, Get("b"));
  EXPECT_EQ(1, g_deletions.load());  // Still pinned.
  EXPECT_EQ(0u, cache_->TotalCharge());  // But no longer in the cache.
  EXPECT_EQ(2, static_cast<TestValue*>(cache_->Value(h))->payload);
  cache_->Release(h);
  EXPECT_EQ(2, g_deletions.load());

  cache_->Erase("never inserted");
  EXPECT_EQ(2, g_deletions.load());
}

TEST_F(CacheTest, InsertReplacesAnEntryWithTheSameKey) {
  MakeCache(100);
  Cache::Handle* old_handle = Insert("k", 1);
  Put("k", 2);
  EXPECT_EQ(2, Get("k"));
  // The replaced value stays valid for its holder, then is freed.
  EXPECT_EQ(1, static_cast<TestValue*>(cache_->Value(old_handle))->payload);
  EXPECT_EQ(0, g_deletions.load());
  cache_->Release(old_handle);
  const std::vector<std::pair<std::string, int>> expected = {{"k", 1}};
  EXPECT_EQ(expected, deleted_);
  EXPECT_EQ(1u, cache_->TotalCharge());
}

TEST_F(CacheTest, ShardsSplitTheCapacity) {
  MakeCache(1600, Cache::kMaxDefaultShardBits);  // 16 shards of 100.
  for (int i = 0; i < 10000; ++i) {
    Put("key" + std::to_string(i), i, 10);
  }
  EXPECT_LE(cache_->TotalCharge(), 1600u);
  // Keys spread over all shards, so nearly the whole capacity is used.
  EXPECT_GE(cache_->TotalCharge(), 1500u);
  int found = 0;
  for (int i = 0; i < 10000; ++i) {
    found += Get("key" + std::to_string(i)) == i ? 1 : 0;
  }
  EXPECT_EQ(static_cast<int>(cache_->TotalCharge() / 10), found);
}

TEST(CacheShardingTest, SmallCachesUseFewerShards) {
  EXPECT_EQ(0, Cache::DefaultShardBits(0));
  EXPECT_EQ(0, Cache::DefaultShardBits(8 * 1024));
  EXPECT_EQ(0, Cache::DefaultShardBits(511 * 1024));
  EXPECT_EQ(1, Cache::DefaultShardBits(512 * 1024));
  EXPECT_EQ(3, Cache::DefaultShardBits(2 * 1024 * 1024));
  EXPECT_EQ(4, Cache::DefaultShardBits(4 * 1024 * 1024));
  EXPECT_EQ(4, Cache::DefaultShardBits(size_t{1} << 40));
}

TEST_F(CacheTest, DestructorFreesEverything) {
  MakeCache(100);
  for (int i = 0; i < 10; ++i) {
    Put("k" + std::to_string(i), i);
  }
  cache_.reset();
  EXPECT_EQ(10, g_deletions.load());
}

TEST_F(CacheTest, NewIdIsUnique) {
  MakeCache(1);
  const uint64_t a = cache_->NewId();
  const uint64_t b = cache_->NewId();
  EXPECT_NE(a, b);
  EXPECT_NE(0u, a);
}

// Many threads looking up, inserting, erasing and holding handles on a small
// key space. Checks that every handle's value belongs to its key, that the
// capacity holds once all handles are released, and that every value is
// freed exactly once. Run under the tsan preset for race detection.
TEST_F(CacheTest, ConcurrentUseIsSafe) {
  g_deleted = nullptr;  // The log is not thread-safe; only count.
  MakeCache(400, Cache::kMaxDefaultShardBits);
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 20000;
  std::atomic<int> inserts{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(t) + 1);
      std::vector<std::pair<Cache::Handle*, std::string>> held;
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kOpsPerThread; ++i) {
        const std::string key = "k" + std::to_string(rng() % 200);
        const unsigned op = rng() % 10;
        if (op < 6) {
          Cache::Handle* h = cache_->Lookup(key);
          if (h != nullptr) {
            ASSERT_EQ(key, static_cast<TestValue*>(cache_->Value(h))->key);
            held.emplace_back(h, key);
          }
        } else if (op < 9) {
          held.emplace_back(
              cache_->Insert(key, new TestValue{key, i}, 1 + rng() % 8, &DeleteTestValue), key);
          inserts.fetch_add(1, std::memory_order_relaxed);
        } else {
          cache_->Erase(key);
        }
        // Hold a few handles across other threads' operations.
        while (held.size() > 4) {
          const size_t victim = rng() % held.size();
          // Still the value it was, whatever happened to the key meanwhile.
          const auto& [handle, handle_key] = held[victim];
          ASSERT_EQ(handle_key, static_cast<TestValue*>(cache_->Value(handle))->key);
          cache_->Release(handle);
          held.erase(held.begin() + static_cast<std::ptrdiff_t>(victim));
        }
      }
      for (const auto& entry : held) {
        cache_->Release(entry.first);
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (auto& thread : threads) {
    thread.join();
  }
  // With every handle released, each shard (16 of 25) is back within its
  // share of the capacity.
  EXPECT_LE(cache_->TotalCharge(), 400u);
  const Cache::Stats stats = cache_->GetStats();
  EXPECT_EQ(static_cast<uint64_t>(inserts.load()), stats.inserts);
  EXPECT_GT(stats.hits, 0u);
  cache_.reset();
  EXPECT_EQ(inserts.load(), g_deletions.load());
}

// ---------------------------------------------------------------------------
// The block cache inside a database.

class BlockCacheDBTest : public test::DBTestBase {
 protected:
  BlockCacheDBTest() {
    options_.statistics = stats_;
    options_.disable_auto_compactions = true;  // Keep the file set fixed.
    options_.block_size = 1024;
    UseNoSyncEnv();
  }

  void Fill(int n) {
    for (int i = 0; i < n; ++i) {
      ASSERT_OK(Put(test::Key(static_cast<uint64_t>(i)), "value" + std::to_string(i)));
    }
    ASSERT_OK(FlushAndSettle());
  }

  void ReadAll(int n) {
    for (int i = 0; i < n; ++i) {
      ASSERT_EQ("value" + std::to_string(i), Get(test::Key(static_cast<uint64_t>(i)))) << i;
    }
  }

  std::shared_ptr<Statistics> stats_ = std::make_shared<Statistics>();
};

TEST_F(BlockCacheDBTest, RepeatedReadsHitTheCacheInsteadOfTheDisk) {
  ASSERT_OK(Reopen());
  Fill(2000);
  stats_->Reset();

  ReadAll(2000);  // First pass: every block comes from the file once.
  const uint64_t reads_after_first_pass = stats_->block_reads.load();
  EXPECT_GT(reads_after_first_pass, 20u);  // Many blocks of about 1 KiB.
  EXPECT_EQ(reads_after_first_pass, stats_->block_cache_misses.load());
  EXPECT_GT(stats_->block_read_bytes.load(), reads_after_first_pass * 900);

  for (int pass = 0; pass < 3; ++pass) {
    const uint64_t hits_before = stats_->block_cache_hits.load();
    ReadAll(2000);
    EXPECT_EQ(reads_after_first_pass, stats_->block_reads.load()) << "pass " << pass;
    EXPECT_EQ(hits_before + 2000, stats_->block_cache_hits.load()) << "pass " << pass;
  }
}

TEST_F(BlockCacheDBTest, WithoutACacheEveryLookupReadsABlock) {
  options_.block_cache_capacity = 0;
  ASSERT_OK(Reopen());
  Fill(500);
  stats_->Reset();
  ReadAll(500);
  ReadAll(500);
  EXPECT_EQ(1000u, stats_->block_reads.load());
  EXPECT_EQ(0u, stats_->block_cache_hits.load());
  EXPECT_EQ(0u, stats_->block_cache_misses.load());
}

TEST_F(BlockCacheDBTest, ASmallCacheKeepsOnlyRecentBlocks) {
  options_.block_cache_capacity = 8 * 1024;  // A handful of 1 KiB blocks.
  ASSERT_OK(Reopen());
  Fill(4000);
  stats_->Reset();
  ReadAll(4000);
  const uint64_t first_pass_reads = stats_->block_reads.load();
  EXPECT_GT(first_pass_reads, 50u);
  ReadAll(4000);
  // A sequential pass over far more blocks than fit leaves only the last few
  // cached, so the second pass reads nearly every block again.
  const uint64_t second_pass_reads = stats_->block_reads.load() - first_pass_reads;
  EXPECT_GE(second_pass_reads, first_pass_reads - 16);
  // Within a pass, the keys of one block are read one after another, so all
  // but the first lookup of each block hit.
  EXPECT_GT(stats_->block_cache_hits.load(), 4 * stats_->block_cache_misses.load());
}

}  // namespace
}  // namespace kvlog
