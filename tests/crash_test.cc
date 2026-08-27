// Kill-the-process tests with compaction running: short versions of the
// stress loops in stress_test.cc that belong in the default suite.

#include <cstdint>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "crash_test_util.h"
#include "db_test_util.h"
#include "test_util.h"

namespace kvlog {
namespace {

class CrashTest : public test::DBTestBase {
 protected:
  // No database is opened here: the helper owns it while it runs.
  void SetUp() override {
    UseNoSyncEnv();
    options_.write_buffer_size = 16 << 10;
    // A process kill is never damage, so recovery must pass the strictest
    // checks and must not have needed point-in-time recovery.
    options_.paranoid_checks = true;
  }
};

// Random synced writes with thresholds that compact constantly, killed
// three times on the same database (once with compactions slowed down so
// the kill is likely to hit one). The first round lets background work
// settle part way, so at least one compaction is sure to finish before a
// kill.
TEST_F(CrashTest, KillsDuringConstantCompactionKeepEveryAcknowledgedOp) {
  const test::CrashEngineConfig engine = test::CrashEngineConfig::CompactConstantly();
  engine.ApplyTo(&options_);
  std::map<std::string, std::string> state;
  uint64_t compactions = 0;
  for (int round = 0; round < 3; ++round) {
    const std::string ack_file = dir_.Sub("acks" + std::to_string(round));
    test::CrashRun run;
    run.seed = 31 + static_cast<uint64_t>(round);
    run.sync = true;
    run.write_buffer_size = options_.write_buffer_size;
    run.engine = engine;
    run.engine.compaction_throttle_us = round == 2 ? 1000 : 0;
    run.kill_after_acks = 400 + 150 * static_cast<uint64_t>(round);
    run.settle_after_op = round == 0 ? 300 : 0;  // About three write buffers in.
    test::CrashRunReport report;
    test::RunRandomWritesAndKill(dbname_, ack_file, run, &report);
    ASSERT_FALSE(HasFatalFailure());
    compactions += report.compactions;
    EXPECT_GT(report.flushes, 0u) << "round " << round;

    ASSERT_OK(Reopen()) << "round " << round;
    EXPECT_GE(test::VerifyAgainstAcks(db_.get(), run.seed, ack_file, state),
              run.kill_after_acks);
    ASSERT_FALSE(HasFailure()) << "round " << round;
    ExpectNoRecoveryNote();
    state = Scan();
    Close();
  }
  EXPECT_GT(compactions, 0u) << "the helper never finished a compaction";
}

// A kill while a manual compaction is writing its outputs: the inputs are
// still the live files, the partial outputs are garbage, and recovery must
// return exactly the data written before the compaction started.
TEST_F(CrashTest, KillInsideAManualCompactionLosesNothing) {
  test::CrashEngineConfig engine = test::CrashEngineConfig::CompactConstantly();
  engine.disable_auto_compactions = true;
  constexpr uint64_t kSeed = 77;
  constexpr uint64_t kOps = 600;
  // The third append: inside the first output's data, after a block or two.
  const bool inside = test::RunCompactAndKill(dbname_, kSeed, kOps, options_.write_buffer_size,
                                              engine, /*stall_at=*/3);
  ASSERT_FALSE(HasFailure());
  ASSERT_TRUE(inside) << "the compaction finished before the kill";
  EXPECT_FALSE(DbFiles(".tmp").empty()) << "the kill did not interrupt a table write";

  // Reopen without automatic compactions, so whatever table files exist
  // after recovery are exactly the ones it kept.
  options_.disable_auto_compactions = true;
  ASSERT_OK(Reopen());
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_TRUE(DbFiles(".tmp").empty()) << "recovery left a partial table behind";
  // The compaction never committed: its level-0 inputs are still there.
  EXPECT_GT(FilesAtLevel(0), 0);
  const std::map<std::string, std::string> model = test::ModelAfterOps(kSeed, kOps);
  ASSERT_EQ(model, Scan());
  for (const auto& [key, value] : model) {
    ASSERT_EQ(value, Get(key));
  }
  // The interrupted compaction can be redone and changes nothing visible.
  ASSERT_OK(db_->CompactRange(nullptr, nullptr));
  EXPECT_EQ(0, FilesAtLevel(0));
  EXPECT_EQ(model, Scan());
  ASSERT_OK(Reopen());
  EXPECT_EQ(model, Scan());
}

}  // namespace
}  // namespace kvlog
