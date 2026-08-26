// Foreground writes, reads and iterators racing background flushes. These
// tests use no sleeps or timing: threads coordinate through atomics and
// joins, and every check holds whatever the interleaving. Run them under
// the tsan preset to check the synchronization itself.

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
#include "kvlog/write_batch.h"
#include "test_util.h"

namespace kvlog {
namespace {

std::string ThreadKey(int thread, uint64_t i) {
  return "t" + std::to_string(thread) + "-" + test::Key(i);
}

// "<version>:" followed by padding, so that memtables fill up quickly.
std::string VersionValue(uint64_t version) {
  return std::to_string(version) + ":" + std::string(40, 'p');
}

uint64_t ParseVersion(const std::string& value) {
  return std::stoull(value.substr(0, value.find(':')));
}

class ConcurrencyTest : public test::DBTestBase {
 protected:
  void SetUp() override {
    UseNoSyncEnv();
    options_.write_buffer_size = 8 << 10;  // A flush every few dozen writes.
    // These tests are about flushes and count the level-0 tables they leave;
    // compaction would merge them. CompactionTest runs compactions
    // concurrently with readers and writers.
    options_.disable_auto_compactions = true;
    ASSERT_OK(Reopen());
  }
};

// Writers overwrite their own keys with increasing versions while the
// background thread flushes constantly and another thread forces flushes.
// Readers check that no key ever goes back to an older version, which would
// mean a flush lost an update or reads looked in the wrong place.
TEST_F(ConcurrencyTest, FlushesRunConcurrentlyWithWritesAndReads) {
  constexpr int kWriters = 4;
  constexpr uint64_t kKeysPerWriter = 50;
  constexpr uint64_t kVersions = 20;
  std::atomic<bool> writers_done{false};
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (uint64_t version = 1; version <= kVersions; ++version) {
        for (uint64_t k = 0; k < kKeysPerWriter; ++k) {
          if (!Put(ThreadKey(w, k), VersionValue(version)).ok()) {
            failures.fetch_add(1);
          }
        }
      }
    });
  }
  std::thread flusher([&] {
    while (!writers_done.load()) {
      if (!db_->Flush().ok()) {
        failures.fetch_add(1);
      }
    }
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937_64 rng(static_cast<uint64_t>(r));
      std::map<std::string, uint64_t> newest_seen;
      while (!writers_done.load()) {
        const int writer = static_cast<int>(rng() % kWriters);
        const std::string key = ThreadKey(writer, rng() % kKeysPerWriter);
        std::string value;
        const Status s = db_->Get(ReadOptions(), key, &value);
        if (s.IsNotFound()) {
          if (newest_seen.contains(key)) {
            failures.fetch_add(1);  // Was visible, then vanished.
          }
          continue;
        }
        if (!s.ok()) {
          failures.fetch_add(1);
          continue;
        }
        const uint64_t version = ParseVersion(value);
        uint64_t& newest = newest_seen[key];
        if (version < newest || version > kVersions) {
          failures.fetch_add(1);
        }
        newest = version;
      }
    });
  }

  for (auto& t : writers) {
    t.join();
  }
  writers_done.store(true);
  flusher.join();
  for (auto& t : readers) {
    t.join();
  }
  ASSERT_EQ(0, failures.load());

  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_GT(FilesAtLevel(0), 5);
  const auto check = [&] {
    const std::map<std::string, std::string> contents = Scan();
    ASSERT_EQ(static_cast<size_t>(kWriters * kKeysPerWriter), contents.size());
    for (const auto& [key, value] : contents) {
      EXPECT_EQ(kVersions, ParseVersion(value)) << key;
    }
  };
  check();
  ASSERT_OK(Reopen());
  check();
}

// Each batch writes the same version to three keys. An iterator is a
// snapshot, so it must see all three at one version, however batches,
// memtable switches and flushes interleave with its creation.
TEST_F(ConcurrencyTest, IteratorsSeeBatchesAtomicallyDuringFlushes) {
  constexpr uint64_t kBatches = 600;
  std::atomic<bool> writer_done{false};
  std::atomic<int> failures{0};
  std::atomic<int> snapshots{0};

  std::thread writer([&] {
    for (uint64_t version = 1; version <= kBatches; ++version) {
      WriteBatch batch;
      for (const char* key : {"a", "b", "c"}) {
        batch.Put(key, VersionValue(version));
      }
      if (!db_->Write(WriteOptions(), batch).ok()) {
        failures.fetch_add(1);
      }
    }
    writer_done.store(true);
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; ++r) {
    readers.emplace_back([&] {
      uint64_t previous = 0;
      while (!writer_done.load()) {
        std::unique_ptr<Iterator> iter = db_->NewIterator(ReadOptions());
        std::vector<uint64_t> versions;
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
          versions.push_back(ParseVersion(std::string(iter->value())));
        }
        if (!iter->status().ok()) {
          failures.fetch_add(1);
          continue;
        }
        snapshots.fetch_add(1);
        if (versions.empty()) {
          continue;  // Created before the first batch.
        }
        if (versions.size() != 3 || versions[0] != versions[1] || versions[1] != versions[2] ||
            versions[0] < previous) {
          failures.fetch_add(1);
        }
        previous = versions[0];
      }
    });
  }
  writer.join();
  for (auto& t : readers) {
    t.join();
  }
  EXPECT_EQ(0, failures.load());
  EXPECT_GT(snapshots.load(), 0);
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_GT(FilesAtLevel(0), 1);
  for (const char* key : {"a", "b", "c"}) {
    EXPECT_EQ(VersionValue(kBatches), Get(key));
  }
}

// Writers queue behind one another and are committed in groups; sync and
// non-sync writes mix, and flush requests cut into the queue.
TEST_F(ConcurrencyTest, GroupCommitKeepsEveryWrite) {
  constexpr int kThreads = 8;
  constexpr uint64_t kWritesPerThread = 300;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (uint64_t i = 0; i < kWritesPerThread; ++i) {
        const bool sync = i % 50 == 0;
        if (!Put(ThreadKey(t, i), VersionValue(i), sync).ok()) {
          failures.fetch_add(1);
        }
        if (t == 0 && i % 100 == 99 && !db_->Flush().ok()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  ASSERT_EQ(0, failures.load());
  const auto check = [&] {
    for (int t = 0; t < kThreads; ++t) {
      for (uint64_t i = 0; i < kWritesPerThread; ++i) {
        ASSERT_EQ(VersionValue(i), Get(ThreadKey(t, i)));
      }
    }
  };
  check();
  ASSERT_OK(Reopen());
  check();
}

// An iterator pins the memtables and table files of its snapshot, so a scan
// that runs while writers keep switching memtables and flushing still
// returns exactly the snapshot.
TEST_F(ConcurrencyTest, IteratorKeepsItsSnapshotWhileFlushesContinue) {
  std::map<std::string, std::string> snapshot;
  for (uint64_t i = 0; i < 300; ++i) {
    ASSERT_OK(Put(test::Key(i), VersionValue(0)));
    snapshot[test::Key(i)] = VersionValue(0);
  }
  std::unique_ptr<Iterator> iter = db_->NewIterator(ReadOptions());
  std::atomic<int> failures{0};
  std::thread writer([&] {
    for (uint64_t version = 1; version <= 10; ++version) {
      for (uint64_t i = 0; i < 300; i += 3) {
        if (!Put(test::Key(i), VersionValue(version)).ok() || !Delete(test::Key(i + 1)).ok()) {
          failures.fetch_add(1);
        }
      }
    }
  });
  // The first half of the scan races the writer; the rest runs after every
  // one of its memtable switches and flushes.
  std::map<std::string, std::string> scanned;
  iter->SeekToFirst();
  for (; iter->Valid() && scanned.size() < 150; iter->Next()) {
    scanned.emplace(iter->key(), iter->value());
  }
  writer.join();
  impl()->TEST_WaitForBackgroundIdle();
  for (; iter->Valid(); iter->Next()) {
    scanned.emplace(iter->key(), iter->value());
  }
  EXPECT_OK(iter->status());
  EXPECT_EQ(snapshot, scanned);
  EXPECT_EQ(0, failures.load());
  iter.reset();
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_EQ(VersionValue(10), Get(test::Key(0)));
  EXPECT_EQ("NOT_FOUND", Get(test::Key(1)));
}

}  // namespace
}  // namespace kvlog
