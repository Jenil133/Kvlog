// Leveled compaction: data is unchanged by it, overwritten and deleted
// entries really leave the disk, tombstones stay exactly as long as they
// hide something, levels >= 1 never overlap, readers keep their files, and
// writers are slowed and stopped when level 0 fills up.
//
// No test depends on timing: background progress is awaited through
// WaitForBackgroundWork() and the test hooks of DBImpl.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/version_set.h"
#include "db_test_util.h"
#include "kvlog/statistics.h"
#include "test_util.h"

namespace kvlog {
namespace {

using test::Key;

// One entry stored in a table file of the current version.
struct StoredEntry {
  std::string user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeValue;
  std::string value;
  int level = 0;
};

// Every entry of every table file the current version lists, read straight
// from the files (not through the DB iterator, which hides old versions and
// tombstones).
std::vector<StoredEntry> StoredEntries(DBImpl* db) {
  const std::shared_ptr<Version> version = db->TEST_CurrentVersion();
  std::vector<std::unique_ptr<Iterator>> iters;
  version->AddIterators(ReadOptions(), &iters);
  // AddIterators yields one iterator per level-0 file, then one per
  // non-empty deeper level.
  std::vector<int> levels(static_cast<size_t>(version->NumFiles(0)), 0);
  for (int level = 1; level < kNumLevels; ++level) {
    if (version->NumFiles(level) > 0) {
      levels.push_back(level);
    }
  }
  EXPECT_EQ(levels.size(), iters.size());

  std::vector<StoredEntry> entries;
  for (size_t i = 0; i < iters.size(); ++i) {
    Iterator* it = iters[i].get();
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      ParsedInternalKey parsed;
      EXPECT_TRUE(ParseInternalKey(it->key(), &parsed));
      StoredEntry entry;
      entry.user_key = std::string(parsed.user_key);
      entry.sequence = parsed.sequence;
      entry.type = parsed.type;
      entry.value = std::string(it->value());
      entry.level = levels[i];
      entries.push_back(std::move(entry));
    }
    EXPECT_OK(it->status());
  }
  return entries;
}

// Table file numbers listed by `version`.
std::set<uint64_t> VersionFiles(const Version& version) {
  std::set<uint64_t> numbers;
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& file : version.files(level)) {
      numbers.insert(file->number);
    }
  }
  return numbers;
}

// Checks that every level >= 1 is a sorted run of files with disjoint
// user-key ranges, and that each file's bounds are ordered.
void ExpectLevelInvariants(const Version& version) {
  const Comparator* const ucmp = BytewiseComparator();
  for (int level = 0; level < kNumLevels; ++level) {
    const auto& files = version.files(level);
    for (const auto& file : files) {
      EXPECT_LE(ucmp->Compare(file->smallest.user_key(), file->largest.user_key()), 0);
    }
    if (level == 0) {
      continue;
    }
    for (size_t i = 1; i < files.size(); ++i) {
      EXPECT_LT(ucmp->Compare(files[i - 1]->largest.user_key(), files[i]->smallest.user_key()), 0)
          << "level " << level << " files " << files[i - 1]->number << " and " << files[i]->number
          << " overlap\n"
          << version.DebugString();
    }
  }
}

uint64_t FileNumberOf(const std::string& path) {
  const std::string name = path.substr(path.find_last_of('/') + 1);
  return std::stoull(name.substr(0, name.find('.')));
}

class CompactionTest : public test::DBTestBase {
 protected:
  CompactionTest() {
    options_.statistics = statistics_;
    options_.write_buffer_size = 8 << 10;
    options_.target_file_size = 8 << 10;
    options_.max_bytes_for_level_base = 32 << 10;
    options_.max_bytes_for_level_multiplier = 4;
    options_.level0_compaction_trigger = 4;
    options_.level0_slowdown_writes_trigger = 8;
    options_.level0_stop_writes_trigger = 12;
  }

  void SetUp() override {
    UseNoSyncEnv();
    ASSERT_OK(Reopen());
  }

  // Applies `ops` random puts and deletes over `num_keys` keys to the
  // database and to model_.
  void RandomWorkload(uint64_t seed, int ops, uint64_t num_keys) {
    std::mt19937_64 rng(seed);
    for (int i = 0; i < ops; ++i) {
      const std::string key = Key(rng() % num_keys);
      if (rng() % 4 == 0) {
        ASSERT_OK(Delete(key));
        model_.erase(key);
      } else {
        const std::string value = test::RandomBytes(&rng, 20 + rng() % 120);
        ASSERT_OK(Put(key, value));
        model_[key] = value;
      }
    }
  }

  // Compares Get and a full scan with model_.
  void ExpectMatchesModel(const char* when, uint64_t num_keys) {
    EXPECT_EQ(model_, Scan()) << when;
    for (uint64_t i = 0; i < num_keys; ++i) {
      const auto it = model_.find(Key(i));
      ASSERT_EQ(it == model_.end() ? "NOT_FOUND" : it->second, Get(Key(i))) << Key(i) << " " << when;
    }
  }

  uint64_t TotalFiles() {
    uint64_t total = 0;
    for (int level = 0; level < kNumLevels; ++level) {
      total += static_cast<uint64_t>(FilesAtLevel(level));
    }
    return total;
  }

  std::shared_ptr<Statistics> statistics_ = std::make_shared<Statistics>();
  std::map<std::string, std::string> model_;
};

TEST_F(CompactionTest, CompactRangeLeavesTheDataUnchanged) {
  options_.disable_auto_compactions = true;
  ASSERT_OK(Reopen());
  constexpr uint64_t kKeys = 600;
  RandomWorkload(1, 6000, kKeys);
  ASSERT_GT(FilesAtLevel(0), 20);
  ExpectMatchesModel("before compaction", kKeys);

  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  EXPECT_EQ(0, FilesAtLevel(0));
  EXPECT_GT(FilesAtLevel(1), 0);
  ExpectMatchesModel("after compaction", kKeys);
  ExpectLevelInvariants(*impl()->TEST_CurrentVersion());

  // Compacting again changes nothing visible either.
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  ExpectMatchesModel("after a second compaction", kKeys);

  ASSERT_OK(Reopen());
  ExpectMatchesModel("after reopen", kKeys);
}

TEST_F(CompactionTest, AutomaticCompactionLeavesTheDataUnchanged) {
  constexpr uint64_t kKeys = 2000;
  RandomWorkload(2, 20000, kKeys);
  db_->WaitForBackgroundWork();
  EXPECT_GT(statistics_->compactions.load(), 0u);
  EXPECT_GT(statistics_->compaction_bytes_read.load(), 0u);
  EXPECT_GT(statistics_->compaction_bytes_written.load(), 0u);
  // Idle means no level is over its limit any more.
  EXPECT_LT(FilesAtLevel(0), options_.level0_compaction_trigger);
  int deeper_files = 0;
  for (int level = 1; level < kNumLevels; ++level) {
    deeper_files += FilesAtLevel(level);
  }
  EXPECT_GT(deeper_files, 0);
  ExpectLevelInvariants(*impl()->TEST_CurrentVersion());
  ExpectMatchesModel("after automatic compactions", kKeys);

  ASSERT_OK(Reopen());
  ExpectMatchesModel("after reopen", kKeys);
}

TEST_F(CompactionTest, OverwrittenAndDeletedKeysAreDroppedFromDisk) {
  options_.disable_auto_compactions = true;
  ASSERT_OK(Reopen());
  constexpr uint64_t kKeys = 300;
  for (int round = 0; round < 10; ++round) {
    for (uint64_t i = 0; i < kKeys; ++i) {
      ASSERT_OK(Put(Key(i), "round" + std::to_string(round) + std::string(50, 'v')));
    }
  }
  for (uint64_t i = 0; i < kKeys; i += 2) {
    ASSERT_OK(Delete(Key(i)));
  }
  ASSERT_OK(db_->Flush());
  // Before: every version and every tombstone is on disk.
  EXPECT_EQ(kKeys * 10 + kKeys / 2, StoredEntries(impl()).size());
  const uint64_t sst_bytes_before = [&] {
    uint64_t total = 0;
    for (const std::string& f : DbFiles(".sst")) total += test::FileSize(f);
    return total;
  }();

  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  impl()->TEST_WaitForBackgroundIdle();

  // After: exactly one entry per live key, holding its newest value, and no
  // tombstones (nothing older exists anywhere).
  const std::vector<StoredEntry> entries = StoredEntries(impl());
  ASSERT_EQ(kKeys / 2, entries.size());
  for (const StoredEntry& entry : entries) {
    EXPECT_EQ(kTypeValue, entry.type) << entry.user_key;
    EXPECT_EQ("round9" + std::string(50, 'v'), entry.value) << entry.user_key;
    EXPECT_EQ(1u, std::stoull(entry.user_key.substr(3)) % 2) << entry.user_key;
  }
  // The files on disk are exactly the ones the version lists.
  std::set<uint64_t> on_disk;
  for (const std::string& f : DbFiles(".sst")) on_disk.insert(FileNumberOf(f));
  EXPECT_EQ(VersionFiles(*impl()->TEST_CurrentVersion()), on_disk);
  uint64_t sst_bytes_after = 0;
  for (const std::string& f : DbFiles(".sst")) sst_bytes_after += test::FileSize(f);
  EXPECT_LT(sst_bytes_after * 10, sst_bytes_before)
      << sst_bytes_after << " bytes after, " << sst_bytes_before << " before";
}

TEST_F(CompactionTest, TombstoneStaysWhileADeeperLevelHoldsTheKey) {
  options_.disable_auto_compactions = true;
  ASSERT_OK(Reopen());
  ASSERT_OK(Put("a", "old-a"));
  ASSERT_OK(Put("b", "old-b"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(impl()->TEST_CompactRange(0, nullptr, nullptr));
  ASSERT_OK(impl()->TEST_CompactRange(1, nullptr, nullptr));
  ASSERT_EQ(1, FilesAtLevel(2));

  // "a" still has a value in level 2; "zz" was never written, and no deeper
  // file's range covers it.
  ASSERT_OK(Delete("a"));
  ASSERT_OK(Delete("zz"));
  ASSERT_OK(Put("c", "new-c"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(impl()->TEST_CompactRange(0, nullptr, nullptr));
  ASSERT_EQ(0, FilesAtLevel(0));
  ASSERT_EQ(1, FilesAtLevel(1));

  bool tombstone_for_a = false;
  bool anything_for_zz = false;
  for (const StoredEntry& entry : StoredEntries(impl())) {
    if (entry.level == 1 && entry.user_key == "a") {
      EXPECT_EQ(kTypeDeletion, entry.type);
      tombstone_for_a = true;
    }
    anything_for_zz |= entry.user_key == "zz";
  }
  EXPECT_TRUE(tombstone_for_a) << "dropping it would resurrect old-a from level 2";
  EXPECT_FALSE(anything_for_zz) << "a tombstone hiding nothing must be dropped";
  EXPECT_EQ("NOT_FOUND", Get("a"));
  EXPECT_EQ("old-b", Get("b"));

  // Merging level 1 into level 2 meets the old value: both go.
  ASSERT_OK(impl()->TEST_CompactRange(1, nullptr, nullptr));
  ASSERT_EQ(0, FilesAtLevel(1));
  for (const StoredEntry& entry : StoredEntries(impl())) {
    EXPECT_NE("a", entry.user_key);
  }
  const std::map<std::string, std::string> expected = {{"b", "old-b"}, {"c", "new-c"}};
  EXPECT_EQ(expected, Scan());
  ASSERT_OK(Reopen());
  EXPECT_EQ(expected, Scan());
}

TEST_F(CompactionTest, CompactRangeOnlyTouchesOverlappingFiles) {
  options_.disable_auto_compactions = true;
  options_.write_buffer_size = 64 << 10;
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 100; ++i) ASSERT_OK(Put(Key(i), "low"));
  ASSERT_OK(db_->Flush());
  for (uint64_t i = 500; i < 600; ++i) ASSERT_OK(Put(Key(i), "high"));
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(2, FilesAtLevel(0));

  const std::string begin = Key(10);
  const std::string end = Key(20);
  ASSERT_OK(db_->CompactRange(&begin, &end));
  EXPECT_EQ(1, FilesAtLevel(0));  // The 500..599 file was left alone.
  EXPECT_EQ(1, FilesAtLevel(1));
  EXPECT_EQ(200u, Scan().size());
}

TEST_F(CompactionTest, LevelsAboveZeroNeverOverlap) {
  options_.write_buffer_size = 16 << 10;
  options_.target_file_size = 4 << 10;
  options_.max_bytes_for_level_base = 16 << 10;
  options_.max_bytes_for_level_multiplier = 2;
  options_.level0_compaction_trigger = 2;
  ASSERT_OK(Reopen());
  constexpr uint64_t kKeys = 3000;
  for (uint64_t seed = 10; seed < 16; ++seed) {
    RandomWorkload(seed, 4000, kKeys);
    // Every version ever installed is checked when it is the current one.
    ExpectLevelInvariants(*impl()->TEST_CurrentVersion());
  }
  db_->WaitForBackgroundWork();
  const std::shared_ptr<Version> version = impl()->TEST_CurrentVersion();
  ExpectLevelInvariants(*version);
  // The workload is big enough to need several levels.
  EXPECT_GT(version->NumFiles(2), 0) << version->DebugString();
  for (int level = 1; level + 1 < kNumLevels; ++level) {
    uint64_t bytes = 0;
    for (const auto& file : version->files(level)) bytes += file->file_size;
    EXPECT_LT(static_cast<double>(bytes),
              static_cast<double>(options_.max_bytes_for_level_base) *
                  std::pow(options_.max_bytes_for_level_multiplier, level - 1))
        << "level " << level;
  }
  ExpectMatchesModel("after a multi-level workload", kKeys);
}

TEST_F(CompactionTest, OpenIteratorKeepsItsFilesAlive) {
  options_.disable_auto_compactions = true;
  options_.write_buffer_size = 64 << 10;
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 200; ++i) ASSERT_OK(Put(Key(i), "first"));
  ASSERT_OK(db_->Flush());
  for (uint64_t i = 0; i < 200; i += 2) ASSERT_OK(Put(Key(i), "second"));
  ASSERT_OK(FlushAndSettle());
  const std::vector<std::string> old_tables = DbFiles(".sst");
  ASSERT_EQ(2u, old_tables.size());

  std::unique_ptr<Iterator> iter = db_->NewIterator(ReadOptions());
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  // Changes made after the iterator was created stay invisible to it.
  for (uint64_t i = 0; i < 200; ++i) ASSERT_OK(Delete(Key(i)));
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_EQ(0, FilesAtLevel(0));

  // The version the iterator reads still needs the old tables.
  const std::vector<std::string> during = DbFiles(".sst");
  for (const std::string& table : old_tables) {
    EXPECT_TRUE(std::find(during.begin(), during.end(), table) != during.end()) << table;
  }
  uint64_t count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    EXPECT_EQ(Key(count), iter->key());
    EXPECT_EQ(count % 2 == 0 ? "second" : "first", iter->value());
    ++count;
  }
  ASSERT_OK(iter->status());
  EXPECT_EQ(200u, count);
  EXPECT_TRUE(Scan().empty());

  // Once the iterator is gone, the next cleanup removes them.
  iter.reset();
  ASSERT_OK(Put("trigger", "cleanup"));
  ASSERT_OK(FlushAndSettle());
  const std::vector<std::string> after = DbFiles(".sst");
  for (const std::string& table : old_tables) {
    EXPECT_TRUE(std::find(after.begin(), after.end(), table) == after.end()) << table;
  }
  std::set<uint64_t> on_disk;
  for (const std::string& f : after) on_disk.insert(FileNumberOf(f));
  EXPECT_EQ(VersionFiles(*impl()->TEST_CurrentVersion()), on_disk);
}

TEST_F(CompactionTest, WritesSlowDownThenStopWhileLevel0IsFull) {
  options_.write_buffer_size = 16 << 10;
  options_.level0_compaction_trigger = 2;
  options_.level0_slowdown_writes_trigger = 3;
  options_.level0_stop_writes_trigger = 4;
  ASSERT_OK(Reopen());
  impl()->TEST_SetAutoCompactionsPaused(true);

  // About 200 KB through a 16 KB memtable: far more level-0 files than the
  // stop trigger allows, so the writer must block until compaction resumes.
  // The keys cycle, so level-0 files overlap and need real merges.
  constexpr uint64_t kWrites = 1500;
  constexpr uint64_t kKeys = 400;
  auto value_of = [](uint64_t i) { return std::to_string(i) + std::string(100, 'x'); };
  std::atomic<int> failures{0};
  std::thread writer([&] {
    for (uint64_t i = 0; i < kWrites; ++i) {
      if (!Put(Key(i % kKeys), value_of(i)).ok()) failures.fetch_add(1);
    }
  });
  impl()->TEST_WaitForStoppedWriter();
  EXPECT_GE(FilesAtLevel(0), options_.level0_stop_writes_trigger);
  EXPECT_GE(statistics_->write_stops.load(), 1u);
  EXPECT_GE(statistics_->write_slowdowns.load(), 1u);
  EXPECT_EQ(0u, statistics_->compactions.load());

  impl()->TEST_SetAutoCompactionsPaused(false);
  writer.join();
  EXPECT_EQ(0, failures.load());
  db_->WaitForBackgroundWork();
  EXPECT_LT(FilesAtLevel(0), options_.level0_compaction_trigger);
  EXPECT_GT(statistics_->compactions.load(), 0u);
  for (uint64_t i = kWrites - kKeys; i < kWrites; ++i) {
    ASSERT_EQ(value_of(i), Get(Key(i % kKeys)));
  }
  std::string stats;
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
  EXPECT_NE(std::string::npos, stats.find("write_stops ")) << stats;
}

TEST_F(CompactionTest, DisabledAutoCompactionsNeverStallWrites) {
  options_.disable_auto_compactions = true;
  options_.write_buffer_size = 16 << 10;
  options_.level0_compaction_trigger = 2;
  options_.level0_slowdown_writes_trigger = 3;
  options_.level0_stop_writes_trigger = 4;
  ASSERT_OK(Reopen());
  const std::string value(100, 'x');
  for (uint64_t i = 0; i < 1500; ++i) ASSERT_OK(Put(Key(i), value));
  db_->WaitForBackgroundWork();
  EXPECT_GT(FilesAtLevel(0), 8);
  EXPECT_EQ(0u, statistics_->compactions.load());
  EXPECT_EQ(0u, statistics_->write_stops.load());
  EXPECT_EQ(0u, statistics_->write_slowdowns.load());
  EXPECT_EQ(static_cast<uint64_t>(FilesAtLevel(0)), statistics_->flushes.load());

  // Manual compaction still works.
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  EXPECT_EQ(0, FilesAtLevel(0));
  EXPECT_EQ(1u, statistics_->compactions.load());
  EXPECT_EQ(1500u, Scan().size());
}

// Writers overwrite their own keys with increasing counters while automatic
// compactions run, another thread keeps forcing full compactions, and
// readers check that no key ever goes back to an older counter (a
// compaction that lost an update or dropped the wrong version would show up
// here) and that scans are sorted and complete. Run under tsan.
TEST_F(CompactionTest, ReadersAndWritersRunDuringCompactions) {
  options_.write_buffer_size = 16 << 10;
  options_.target_file_size = 4 << 10;
  options_.max_bytes_for_level_base = 16 << 10;
  options_.level0_compaction_trigger = 2;
  ASSERT_OK(Reopen());

  constexpr int kWriters = 3;
  constexpr uint64_t kKeysPerWriter = 60;
  constexpr uint64_t kRounds = 61;  // Not a deleting round, so every key ends live.
  auto key_of = [](int w, uint64_t k) { return "w" + std::to_string(w) + "-" + Key(k); };
  auto value_of = [](uint64_t round) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08llu", static_cast<unsigned long long>(round));
    return std::string(buf) + std::string(40, 'p');
  };
  auto round_of = [](std::string_view value) { return std::stoull(std::string(value.substr(0, 8))); };

  std::atomic<int> writers_running{kWriters};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (uint64_t round = 1; round <= kRounds; ++round) {
        for (uint64_t k = 0; k < kKeysPerWriter; ++k) {
          // Every fifth round deletes a third of the keys; the next round
          // brings them back.
          const Status s = (round % 5 == 0 && k % 3 == 0) ? Delete(key_of(w, k))
                                                          : Put(key_of(w, k), value_of(round));
          if (!s.ok()) failures.fetch_add(1);
        }
      }
      writers_running.fetch_sub(1);
    });
  }
  threads.emplace_back([&] {
    while (writers_running.load() > 0) {
      if (!db_->CompactRange(nullptr, nullptr).ok()) failures.fetch_add(1);
    }
  });
  for (int r = 0; r < 2; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937_64 rng(static_cast<uint64_t>(r) + 100);
      std::map<std::string, uint64_t> newest_seen;
      while (writers_running.load() > 0) {
        const std::string key = key_of(static_cast<int>(rng() % kWriters), rng() % kKeysPerWriter);
        std::string value;
        const Status s = db_->Get(ReadOptions(), key, &value);
        if (s.ok()) {
          const uint64_t round = round_of(value);
          if (round < newest_seen[key]) failures.fetch_add(1);
          newest_seen[key] = round;
        } else if (!s.IsNotFound()) {
          failures.fetch_add(1);
        }
        if (rng() % 16 == 0) {
          std::unique_ptr<Iterator> it = db_->NewIterator(ReadOptions());
          std::string previous;
          uint64_t count = 0;
          for (it->SeekToFirst(); it->Valid(); it->Next()) {
            const std::string current(it->key());
            if (count > 0 && current <= previous) failures.fetch_add(1);
            if (it->value().size() != 48) failures.fetch_add(1);
            previous = current;
            ++count;
          }
          if (!it->status().ok() || count > kWriters * kKeysPerWriter) failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_EQ(0, failures.load());

  db_->WaitForBackgroundWork();
  ExpectLevelInvariants(*impl()->TEST_CurrentVersion());
  EXPECT_GT(statistics_->compactions.load(), 0u);
  for (int w = 0; w < kWriters; ++w) {
    for (uint64_t k = 0; k < kKeysPerWriter; ++k) {
      ASSERT_EQ(value_of(kRounds), Get(key_of(w, k)));
    }
  }
}

TEST_F(CompactionTest, StatsPropertyReportsCompactionsAndCounters) {
  options_.disable_auto_compactions = true;
  options_.write_buffer_size = 64 << 10;
  ASSERT_OK(Reopen());
  for (int round = 0; round < 3; ++round) {
    for (uint64_t i = 0; i < 100; ++i) ASSERT_OK(Put(Key(i), "value"));
    ASSERT_OK(db_->Flush());
  }
  EXPECT_EQ(3u, statistics_->flushes.load());
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  EXPECT_EQ(1u, statistics_->compactions.load());
  EXPECT_GT(statistics_->compaction_bytes_read.load(), statistics_->compaction_bytes_written.load());

  std::string stats;
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
  EXPECT_NE(std::string::npos, stats.find("statistics:\n")) << stats;
  EXPECT_NE(std::string::npos, stats.find(statistics_->ToString())) << stats;
  EXPECT_NE(std::string::npos, stats.find("compactions 1\n")) << stats;

  // Without Options::statistics the property still works, minus counters.
  options_.statistics = nullptr;
  ASSERT_OK(Reopen());
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
  EXPECT_EQ(std::string::npos, stats.find("statistics:")) << stats;
}

TEST_F(CompactionTest, OutOfRangeOptionsAreClamped) {
  options_.level0_compaction_trigger = 0;
  options_.level0_slowdown_writes_trigger = -3;
  options_.level0_stop_writes_trigger = 1;
  options_.max_bytes_for_level_base = 0;
  options_.max_bytes_for_level_multiplier = -1;
  options_.target_file_size = 0;
  options_.compaction_throttle_us = 50'000'000;
  options_.bloom_bits_per_key = -5;
  ASSERT_OK(Reopen());
  const Options& used = impl()->TEST_Options();
  EXPECT_EQ(1, used.level0_compaction_trigger);
  EXPECT_EQ(1, used.level0_slowdown_writes_trigger);
  EXPECT_EQ(1, used.level0_stop_writes_trigger);
  EXPECT_EQ(1024u, used.max_bytes_for_level_base);
  EXPECT_EQ(2, used.max_bytes_for_level_multiplier);
  EXPECT_EQ(1024u, used.target_file_size);
  EXPECT_EQ(1'000'000u, used.compaction_throttle_us);
  EXPECT_EQ(0, used.bloom_bits_per_key);

  options_ = Options();
  options_.create_if_missing = true;
  options_.env = impl()->TEST_Options().env;
  options_.level0_compaction_trigger = 5000;
  options_.level0_slowdown_writes_trigger = 2;
  options_.max_bytes_for_level_multiplier = 1000;
  options_.bloom_bits_per_key = 1000;
  ASSERT_OK(Reopen());
  EXPECT_EQ(1000, impl()->TEST_Options().level0_compaction_trigger);
  EXPECT_EQ(1000, impl()->TEST_Options().level0_slowdown_writes_trigger);
  EXPECT_EQ(1000, impl()->TEST_Options().level0_stop_writes_trigger);
  EXPECT_EQ(100, impl()->TEST_Options().max_bytes_for_level_multiplier);
  EXPECT_EQ(64, impl()->TEST_Options().bloom_bits_per_key);

  // Defaults are already in range and stay as they are.
  const Options defaults;
  options_ = Options();
  options_.create_if_missing = true;
  options_.env = impl()->TEST_Options().env;
  ASSERT_OK(Reopen());
  EXPECT_EQ(defaults.level0_compaction_trigger, impl()->TEST_Options().level0_compaction_trigger);
  EXPECT_EQ(defaults.level0_stop_writes_trigger, impl()->TEST_Options().level0_stop_writes_trigger);
  EXPECT_EQ(defaults.max_bytes_for_level_base, impl()->TEST_Options().max_bytes_for_level_base);
  EXPECT_EQ(defaults.target_file_size, impl()->TEST_Options().target_file_size);
  EXPECT_EQ(defaults.block_cache_capacity, impl()->TEST_Options().block_cache_capacity);
}

TEST_F(CompactionTest, ThrottledCompactionProducesTheSameResult) {
  options_.disable_auto_compactions = true;
  options_.compaction_throttle_us = 100;
  ASSERT_OK(Reopen());
  constexpr uint64_t kKeys = 300;
  RandomWorkload(7, 2000, kKeys);
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  EXPECT_EQ(0, FilesAtLevel(0));
  ExpectMatchesModel("after a throttled compaction", kKeys);
}

TEST_F(CompactionTest, CompactionOutputIsSplitAtTargetFileSize) {
  options_.disable_auto_compactions = true;
  options_.target_file_size = 4 << 10;
  ASSERT_OK(Reopen());
  const std::string value(200, 'v');
  for (uint64_t i = 0; i < 400; ++i) ASSERT_OK(Put(Key(i), value));  // About 85 KB.
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  const std::shared_ptr<Version> version = impl()->TEST_CurrentVersion();
  ASSERT_GE(version->NumFiles(1), 15) << version->DebugString();
  for (size_t i = 0; i + 1 < version->files(1).size(); ++i) {
    // Each file is cut once it reaches the target; the last entry and the
    // index/footer can push it a little over.
    EXPECT_GE(version->files(1)[i]->file_size, 4u << 10);
    EXPECT_LT(version->files(1)[i]->file_size, 6u << 10);
  }
  ExpectLevelInvariants(*version);
  EXPECT_EQ(400u, Scan().size());
}

// The read-path features working on compaction outputs: every
// table a compaction writes carries a Bloom filter, so missing keys rarely
// read a data block; point lookups fill the block cache, so repeated reads
// stop touching the files; and kvlog.stats reports all of it.
TEST_F(CompactionTest, CompactionOutputsCarryFiltersAndReadsHitTheBlockCache) {
  options_.block_cache_capacity = 4 << 20;
  options_.bloom_bits_per_key = 10;
  options_.write_buffer_size = 64 << 10;
  ASSERT_OK(Reopen());
  constexpr uint64_t kKeys = 3000;
  for (int round = 0; round < 3; ++round) {
    for (uint64_t i = 0; i < kKeys; ++i) {
      ASSERT_OK(Put(Key(i), "r" + std::to_string(round) + std::string(80, 'v')));
    }
  }
  db_->WaitForBackgroundWork();
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  ASSERT_EQ(0, FilesAtLevel(0));  // Every table is now a compaction output.
  ASSERT_GT(statistics_->compactions.load(), 0u);
  ASSERT_GT(TotalFiles(), 3u);

  // Missing keys that sort between stored ones, so key ranges and indexes
  // admit them and only the filter can say no.
  statistics_->Reset();
  constexpr uint64_t kMissing = 2000;
  for (uint64_t i = 0; i < kMissing; ++i) {
    ASSERT_EQ("NOT_FOUND", Get(Key(i) + "-missing"));
  }
  const uint64_t checks = statistics_->bloom_checks.load();
  // Nearly every lookup reaches a table (a few fall in gaps between files).
  EXPECT_GE(checks, kMissing * 9 / 10);
  EXPECT_EQ(checks, statistics_->bloom_useful.load() + statistics_->bloom_false_positives.load());
  const double touched =
      static_cast<double>(statistics_->block_reads.load() + statistics_->block_cache_hits.load()) /
      static_cast<double>(checks);
  EXPECT_LE(touched, 0.02);
  std::printf(
      "[ compacted ] %llu missing-key lookups, %llu filter checks, %llu useful, %llu false "
      "positives, %llu data blocks touched (%.4f of checks)\n",
      static_cast<unsigned long long>(kMissing), static_cast<unsigned long long>(checks),
      static_cast<unsigned long long>(statistics_->bloom_useful.load()),
      static_cast<unsigned long long>(statistics_->bloom_false_positives.load()),
      static_cast<unsigned long long>(statistics_->block_reads.load() +
                                      statistics_->block_cache_hits.load()),
      touched);

  // Present keys: the first pass reads blocks into the cache, the second
  // finds every block there.
  constexpr uint64_t kReads = 500;
  const auto read_pass = [&] {
    for (uint64_t i = 0; i < kReads; ++i) {
      ASSERT_EQ("r2" + std::string(80, 'v'), Get(Key(i * 5)));
    }
  };
  statistics_->Reset();
  read_pass();
  EXPECT_GT(statistics_->block_cache_misses.load(), 0u);
  const uint64_t reads_after_first_pass = statistics_->block_reads.load();
  const uint64_t hits_after_first_pass = statistics_->block_cache_hits.load();
  read_pass();
  EXPECT_EQ(reads_after_first_pass, statistics_->block_reads.load());
  EXPECT_EQ(hits_after_first_pass + kReads, statistics_->block_cache_hits.load());
  std::printf("[ compacted ] 2 x %llu reads of present keys: %llu block reads, %llu cache hits, "
              "%llu cache misses\n",
              static_cast<unsigned long long>(kReads),
              static_cast<unsigned long long>(statistics_->block_reads.load()),
              static_cast<unsigned long long>(statistics_->block_cache_hits.load()),
              static_cast<unsigned long long>(statistics_->block_cache_misses.load()));

  std::string stats;
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
  EXPECT_NE(std::string::npos, stats.find("block cache: capacity 4194304")) << stats;
  EXPECT_NE(std::string::npos, stats.find("bloom_useful ")) << stats;
  EXPECT_NE(std::string::npos,
            stats.find("block_cache_hits " + std::to_string(statistics_->block_cache_hits.load())))
      << stats;
}

}  // namespace
}  // namespace kvlog
