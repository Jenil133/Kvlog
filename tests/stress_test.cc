// Long randomized and crash-loop tests, labelled "stress" in ctest and run
// separately from the default suite: ctest --preset <name>-stress.

#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "crash_test_util.h"
#include "db_test_util.h"
#include "kvlog/write_batch.h"
#include "test_util.h"

namespace kvlog {
namespace {

class StressTest : public test::DBTestBase {};

// Random puts, deletes, batches, reads, seeks, flushes and reopens, checked
// step by step against a std::map.
TEST_F(StressTest, RandomOperationsMatchAModel) {
  UseNoSyncEnv();
  options_.write_buffer_size = 8 << 10;
  ASSERT_OK(Reopen());
  std::mt19937_64 rng(4242);
  std::map<std::string, std::string> model;
  const auto random_key = [&] {
    // Mostly a shared key space, sometimes arbitrary binary keys.
    return rng() % 10 == 0 ? test::RandomBytes(&rng, rng() % 8) : test::Key(rng() % 3000);
  };
  int flushes = 0;
  int reopens = 0;
  for (int step = 0; step < 40000; ++step) {
    const uint64_t dice = rng() % 1000;
    if (dice < 400) {
      const std::string key = random_key();
      const std::string value = test::RandomBytes(&rng, rng() % 120);
      ASSERT_OK(Put(key, value));
      model[key] = value;
    } else if (dice < 600) {
      const std::string key = random_key();
      ASSERT_OK(Delete(key));
      model.erase(key);
    } else if (dice < 700) {
      WriteBatch batch;
      std::map<std::string, std::string> staged = model;
      for (uint64_t i = 0, n = 1 + rng() % 10; i < n; ++i) {
        const std::string key = random_key();
        if (rng() % 3 == 0) {
          batch.Delete(key);
          staged.erase(key);
        } else {
          const std::string value = test::RandomBytes(&rng, rng() % 50);
          batch.Put(key, value);
          staged[key] = value;
        }
      }
      ASSERT_OK(db_->Write(WriteOptions(), batch));
      model = std::move(staged);
    } else if (dice < 950) {
      const std::string key = random_key();
      const auto it = model.find(key);
      ASSERT_EQ(it == model.end() ? "NOT_FOUND" : it->second, Get(key)) << "step " << step;
    } else if (dice < 994) {
      const std::string target = random_key();
      std::unique_ptr<Iterator> iter = db_->NewIterator(ReadOptions());
      iter->Seek(target);
      auto expected = model.lower_bound(target);
      for (int i = 0; i < 20 && expected != model.end(); ++i, ++expected, iter->Next()) {
        ASSERT_TRUE(iter->Valid()) << "step " << step;
        ASSERT_EQ(expected->first, iter->key()) << "step " << step;
        ASSERT_EQ(expected->second, iter->value()) << "step " << step;
      }
      if (expected == model.end()) {
        ASSERT_FALSE(iter->Valid()) << "step " << step;
      }
      ASSERT_OK(iter->status());
    } else if (dice < 998) {
      ASSERT_OK(db_->Flush());
      ++flushes;
    } else {
      ASSERT_OK(Reopen());
      ++reopens;
    }
  }
  EXPECT_GT(flushes, 50);
  EXPECT_GT(reopens, 30);
  EXPECT_EQ(model, Scan());
  ASSERT_OK(Reopen());
  EXPECT_EQ(model, Scan());
}

// Kills the crash helper again and again on one database, at a different
// point of its write stream each time (and so at arbitrary points of its
// background flushes), and checks after every kill that recovery kept
// every acknowledged op and invented nothing.
TEST_F(StressTest, CrashLoopKeepsEveryAcknowledgedOp) {
  UseNoSyncEnv();
  Close();
  // A process kill is never damage: every reopen must pass the strictest
  // checks, so a kill tail misread as damage fails here instead of being
  // quietly cut away by point-in-time recovery.
  options_.paranoid_checks = true;
  std::mt19937_64 rng(99);
  std::map<std::string, std::string> state;  // Verified contents so far.
  uint64_t total_acked = 0;
  constexpr int kRounds = 40;
  for (int round = 0; round < kRounds; ++round) {
    const std::string ack_file = dir_.Sub("acks" + std::to_string(round));
    test::CrashRun run;
    run.seed = 1000 + static_cast<uint64_t>(round);
    if (round % 10 == 9) {
      // Now and then the honest configuration: every op synced to disk.
      run.sync = true;
      run.skip_fs_sync = false;
      run.kill_after_acks = 20 + rng() % 30;
    } else {
      run.kill_after_acks = 50 + rng() % 2500;
    }
    test::RunRandomWritesAndKill(dbname_, ack_file, run);
    ASSERT_OK(Reopen()) << "round " << round;
    total_acked += test::VerifyAgainstAcks(db_.get(), run.seed, ack_file, state);
    ASSERT_FALSE(HasFailure()) << "round " << round;
    ExpectNoRecoveryNote();
    state = Scan();
    Close();
  }
  std::printf("crash loop: %d kills, %llu acknowledged ops verified\n", kRounds,
              static_cast<unsigned long long>(total_acked));
}

// The crash loop again, with compaction thresholds so small that the
// helper flushes every few dozen ops and compacts level 0 every couple of
// flushes (level 1 spills into level 2 as well), every op synced before it
// is acknowledged. A third of the rounds slow each compaction down with
// compaction_throttle_us, so many kills land while one is running. After
// every kill the database must open, hold every acknowledged op (the one
// in-flight op may go either way), invent nothing and scan in order.
TEST_F(StressTest, CompactionCrashLoopKeepsEveryAcknowledgedOp) {
  UseNoSyncEnv();
  options_.write_buffer_size = 16 << 10;
  const test::CrashEngineConfig engine = test::CrashEngineConfig::CompactConstantly();
  engine.ApplyTo(&options_);  // The verifying opens compact the same way.
  options_.paranoid_checks = true;  // A process kill is never damage.
  Close();
  std::mt19937_64 rng(2026);
  std::map<std::string, std::string> state;
  uint64_t total_acked = 0;
  uint64_t total_flushes = 0;
  uint64_t total_compactions = 0;
  int rounds_with_compactions = 0;
  int kills_mid_table_write = 0;
  constexpr int kRounds = 120;
  for (int round = 0; round < kRounds; ++round) {
    const std::string ack_file = dir_.Sub("acks" + std::to_string(round));
    test::CrashRun run;
    run.seed = 5000 + static_cast<uint64_t>(round);
    run.sync = true;
    run.write_buffer_size = options_.write_buffer_size;
    run.engine = engine;
    run.engine.compaction_throttle_us = round % 3 == 0 ? 0 : (round % 3 == 1 ? 200 : 1000);
    if (round % 20 == 19) {
      // Real file-system syncs now and then; slow, so fewer ops.
      run.skip_fs_sync = false;
      run.kill_after_acks = 20 + rng() % 40;
    } else {
      run.kill_after_acks = 500 + rng() % 1500;
      // Several write buffers in: makes sure these rounds finish flushes and
      // a level-0 compaction; the kill comes later, at a random point.
      run.settle_after_op = 450;
    }
    test::CrashRunReport report;
    test::RunRandomWritesAndKill(dbname_, ack_file, run, &report);
    ASSERT_FALSE(HasFatalFailure()) << "round " << round;
    if (run.settle_after_op != 0) {
      EXPECT_GT(report.compactions, 0u) << "round " << round;
    }
    total_flushes += report.flushes;
    total_compactions += report.compactions;
    rounds_with_compactions += report.compactions > 0 ? 1 : 0;
    // A .tmp file is a table (flush or compaction output) the kill
    // interrupted; recovery must ignore and remove it.
    kills_mid_table_write += DbFiles(".tmp").empty() ? 0 : 1;

    ASSERT_OK(Reopen()) << "round " << round;
    total_acked += test::VerifyAgainstAcks(db_.get(), run.seed, ack_file, state);
    ASSERT_FALSE(HasFailure()) << "round " << round;
    std::string stats;
    ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
    EXPECT_EQ(std::string::npos, stats.find("background error")) << stats;
    ExpectNoRecoveryNote();
    state = Scan();
    Close();
  }
  std::printf(
      "compaction crash loop: %d kills, %llu acknowledged ops verified; helpers finished %llu "
      "flushes and %llu compactions (%d rounds compacted); %d kills interrupted a table write\n",
      kRounds, static_cast<unsigned long long>(total_acked),
      static_cast<unsigned long long>(total_flushes),
      static_cast<unsigned long long>(total_compactions), rounds_with_compactions,
      kills_mid_table_write);
  EXPECT_GT(total_flushes, static_cast<uint64_t>(kRounds));
  EXPECT_GT(rounds_with_compactions, kRounds / 3);
}

// Kills the helper while a manual compaction of the whole key range is
// writing its output files, round after round on one database, each time at
// a different append (so in different output files and blocks). Every write
// was acknowledged before the compaction started, so the recovered contents
// must match the model exactly, and the compaction must not have committed.
TEST_F(StressTest, KillsInsideManualCompactionsLoseNothing) {
  UseNoSyncEnv();
  options_.write_buffer_size = 16 << 10;
  test::CrashEngineConfig engine = test::CrashEngineConfig::CompactConstantly();
  engine.disable_auto_compactions = true;  // Only the manual compaction runs.
  Close();
  options_.disable_auto_compactions = true;  // Level 0 after a reopen is what recovery kept.
  std::map<std::string, std::string> model;
  int kills_inside = 0;
  constexpr int kRounds = 30;
  for (int round = 0; round < kRounds; ++round) {
    const uint64_t seed = 9000 + static_cast<uint64_t>(round);
    const uint64_t ops = 600;
    const uint64_t stall_at = 1 + static_cast<uint64_t>(round) % 10;
    const bool inside = test::RunCompactAndKill(dbname_, seed, ops, options_.write_buffer_size,
                                                engine, stall_at);
    ASSERT_FALSE(HasFailure()) << "round " << round;
    ASSERT_TRUE(inside) << "round " << round << ": the compaction finished in fewer than "
                        << stall_at << " appends";
    kills_inside += 1;
    model = test::ModelAfterOps(seed, ops, std::move(model));
    ASSERT_OK(Reopen()) << "round " << round;
    EXPECT_GT(FilesAtLevel(0), 0) << "round " << round;  // The compaction never committed.
    ASSERT_EQ(model, Scan()) << "round " << round;
    for (const auto& [key, value] : model) {
      ASSERT_EQ(value, Get(key)) << "round " << round;
    }
    // Finish the job the kill interrupted, so level 0 does not pile up
    // across rounds, and check the result once more.
    ASSERT_OK(db_->CompactRange(nullptr, nullptr)) << "round " << round;
    ASSERT_EQ(model, Scan()) << "round " << round;
    Close();
  }
  std::printf("manual compaction kills: %d of %d landed inside the compaction\n", kills_inside,
              kRounds);
}

// The concurrency checks of concurrency_test.cc at a larger scale: more
// writers and versions, readers checking per-key monotonic reads, and
// iterator threads checking sorted, error-free scans, all while the
// background thread flushes a small write buffer.
TEST_F(StressTest, ConcurrentWritersReadersAndFlushes) {
  UseNoSyncEnv();
  options_.write_buffer_size = 16 << 10;
  ASSERT_OK(Reopen());
  constexpr int kWriters = 6;
  constexpr uint64_t kKeys = 100;
  constexpr uint64_t kVersions = 60;
  const auto key_of = [](int writer, uint64_t k) {
    return "w" + std::to_string(writer) + "-" + test::Key(k);
  };
  const auto value_of = [](uint64_t version) {
    return std::to_string(version) + ":" + std::string(60, 'v');
  };
  const auto version_of = [](std::string_view value) {
    return std::stoull(std::string(value.substr(0, value.find(':'))));
  };

  std::atomic<bool> done{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (uint64_t version = 1; version <= kVersions; ++version) {
        for (uint64_t k = 0; k < kKeys; ++k) {
          if (!Put(key_of(w, k), value_of(version), /*sync=*/k == 0 && version % 20 == 0).ok()) {
            failures.fetch_add(1);
          }
        }
      }
    });
  }
  std::vector<std::thread> observers;
  for (int r = 0; r < 3; ++r) {
    observers.emplace_back([&, r] {
      std::mt19937_64 local(static_cast<uint64_t>(r) + 1);
      std::map<std::string, uint64_t> newest_seen;
      while (!done.load()) {
        const std::string key = key_of(static_cast<int>(local() % kWriters), local() % kKeys);
        std::string value;
        const Status s = db_->Get(ReadOptions(), key, &value);
        if (s.IsNotFound()) {
          failures.fetch_add(newest_seen.contains(key) ? 1 : 0);
          continue;
        }
        const uint64_t version = s.ok() ? version_of(value) : 0;
        if (!s.ok() || version < newest_seen[key]) {
          failures.fetch_add(1);
        }
        newest_seen[key] = version;
      }
    });
  }
  observers.emplace_back([&] {
    while (!done.load()) {
      std::unique_ptr<Iterator> iter = db_->NewIterator(ReadOptions());
      std::string previous;
      bool first = true;
      for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
        if (!first && !(previous < iter->key())) {
          failures.fetch_add(1);
        }
        first = false;
        previous.assign(iter->key());
      }
      if (!iter->status().ok()) {
        failures.fetch_add(1);
      }
    }
  });

  for (auto& t : threads) {
    t.join();
  }
  done.store(true);
  for (auto& t : observers) {
    t.join();
  }
  ASSERT_EQ(0, failures.load());
  ASSERT_OK(Reopen());
  const std::map<std::string, std::string> contents = Scan();
  ASSERT_EQ(static_cast<size_t>(kWriters) * kKeys, contents.size());
  for (const auto& [key, value] : contents) {
    EXPECT_EQ(kVersions, version_of(value)) << key;
  }
}

}  // namespace
}  // namespace kvlog
