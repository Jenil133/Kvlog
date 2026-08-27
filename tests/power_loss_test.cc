// Power-loss tests. A process crash keeps everything handed to the operating
// system, so kill tests cannot tell whether the database syncs the right
// files and directories in the right order. These tests stop the "machine"
// at every durability operation (file sync, directory sync, rename, create,
// unlink) of a workload that writes, switches memtables, flushes, compacts,
// rolls the MANIFEST over and reopens, then throw away everything that was
// not durable (PowerLossEnv) and check what recovery restores:
//
//  - Open succeeds, even with paranoid_checks: power loss is not damage.
//  - The contents equal the state after some prefix of the operations that
//    includes every write acknowledged with sync == true and every write
//    acknowledged before a Flush() or a clean close that completed.

#include <sys/stat.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kvlog/db.h"
#include "kvlog/statistics.h"
#include "power_loss_env.h"
#include "test_util.h"

namespace kvlog {
namespace {

struct Op {
  enum class Kind { kPut, kDelete, kFlush };
  Kind kind = Kind::kPut;
  std::string key;
  std::string value;
  bool sync = false;
};

std::vector<Op> MakeOps(size_t count, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Op> ops;
  for (size_t i = 0; i < count; ++i) {
    Op op;
    if (i % 30 == 29) {
      op.kind = Op::Kind::kFlush;
    } else {
      op.kind = rng() % 7 == 0 ? Op::Kind::kDelete : Op::Kind::kPut;
      op.key = test::Key(rng() % 40);
      op.value = "v" + std::to_string(i) + "-" + std::string(100 + rng() % 100, 'p');
      op.sync = rng() % 2 == 0;
    }
    ops.push_back(std::move(op));
  }
  return ops;
}

std::map<std::string, std::string> ModelAfter(const std::vector<Op>& ops, size_t prefix) {
  std::map<std::string, std::string> model;
  for (size_t i = 0; i < prefix; ++i) {
    if (ops[i].kind == Op::Kind::kPut) {
      model[ops[i].key] = ops[i].value;
    } else if (ops[i].kind == Op::Kind::kDelete) {
      model.erase(ops[i].key);
    }
  }
  return model;
}

Options EngineOptions(Env* env, bool manifest_rolls_over_every_update) {
  Options options;
  options.env = env;
  options.create_if_missing = true;
  options.write_buffer_size = 4096;  // A memtable switch every ~25 writes.
  options.level0_compaction_trigger = 2;
  options.max_bytes_for_level_base = 4096;
  options.target_file_size = 2048;
  // Alternate between a MANIFEST that is replaced on every update and one
  // that is appended to, so both kinds of commit are interrupted.
  options.max_manifest_file_size = manifest_rolls_over_every_update ? 1 : 64 << 10;
  options.statistics = std::make_shared<Statistics>();
  return options;
}

struct RunResult {
  bool crashed = false;
  size_t required = 0;   // Ops that must survive: [0, required).
  size_t attempted = 0;  // Ops that may have survived: [0, attempted).
  uint64_t operations = 0;
  uint64_t flushes = 0;
  uint64_t compactions = 0;
};

// Runs the workload against a database in `dbname`, losing power at
// durability operation `crash_at` (0: never). The first half of the ops is
// followed by a clean close and a reopen, so Open's own commit is covered.
RunResult RunWorkload(const std::vector<Op>& ops, const std::string& dbname,
                      const std::string& scratch, uint64_t crash_at, bool manifest_rollover) {
  test::PowerLossEnv env(dbname, scratch);
  env.CrashAtOperation(crash_at);
  const Options options = EngineOptions(&env, manifest_rollover);
  RunResult result;
  std::unique_ptr<DB> db;
  size_t acked_unsynced_from = 0;  // First acked op not yet known durable.
  const auto run = [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const Op& op = ops[i];
      result.attempted = i + 1;
      Status s;
      WriteOptions write_options;
      write_options.sync = op.sync;
      switch (op.kind) {
        case Op::Kind::kPut:
          s = db->Put(write_options, op.key, op.value);
          break;
        case Op::Kind::kDelete:
          s = db->Delete(write_options, op.key);
          break;
        case Op::Kind::kFlush:
          s = db->Flush();
          break;
      }
      if (!s.ok()) {
        return false;
      }
      if (op.sync || op.kind == Op::Kind::kFlush) {
        result.required = i + 1;  // Everything before it is durable too.
      }
      acked_unsynced_from = i + 1;
    }
    return true;
  };

  const size_t half = ops.size() / 2;
  bool ok = DB::Open(options, dbname, &db).ok() && run(0, half);
  if (ok) {
    db.reset();  // A clean close syncs the log.
    if (!env.crashed()) {
      result.required = acked_unsynced_from;
    }
    ok = !env.crashed() && DB::Open(options, dbname, &db).ok() && run(half, ops.size());
  }
  db.reset();
  result.crashed = env.crashed();
  result.operations = env.operations();
  result.flushes = options.statistics->flushes.load();
  result.compactions = options.statistics->compactions.load();
  env.SimulatePowerLoss();
  return result;
}

// Opens what power loss left and checks it against the workload.
void VerifyAfterPowerLoss(const std::vector<Op>& ops, const std::string& dbname,
                          const RunResult& run, uint64_t crash_at) {
  test::NoSyncEnv env;
  Options options;
  options.env = &env;
  options.paranoid_checks = true;
  options.create_if_missing = true;  // Power may have failed while creating it.
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, dbname, &db);
  ASSERT_TRUE(s.ok()) << "crash at operation " << crash_at << ": " << s.ToString();
  const std::map<std::string, std::string> contents = test::ScanAll(db.get());
  for (size_t prefix = run.required; prefix <= run.attempted; ++prefix) {
    if (contents == ModelAfter(ops, prefix)) {
      return;
    }
  }
  ADD_FAILURE() << "crash at operation " << crash_at << ": contents (" << contents.size()
                << " keys) match no state between op " << run.required << " (required) and op "
                << run.attempted << " (attempted)";
}

// Crashes at every durability operation of the workload in turn.
void SweepCrashPoints(size_t num_ops, uint64_t seed, bool manifest_rollover) {
  const std::vector<Op> ops = MakeOps(num_ops, seed);
  uint64_t crash_points = 0;
  RunResult full;
  for (uint64_t crash_at = 1;; ++crash_at) {
    test::TempDir dir;
    const std::string dbname = dir.Sub("db");
    const std::string scratch = dir.Sub("scratch");
    ASSERT_EQ(0, ::mkdir(dbname.c_str(), 0755));
    ASSERT_EQ(0, ::mkdir(scratch.c_str(), 0755));
    const RunResult run = RunWorkload(ops, dbname, scratch, crash_at, manifest_rollover);
    ASSERT_NO_FATAL_FAILURE(VerifyAfterPowerLoss(ops, dbname, run, crash_at));
    if (!run.crashed) {
      // The workload finished before reaching this operation: every point
      // has been tried. (Background work makes the count vary slightly.)
      full = run;
      break;
    }
    ++crash_points;
    ASSERT_LT(crash_at, 100000u) << "workload never finishes";
  }
  EXPECT_GT(full.flushes, 5u);
  EXPECT_GT(full.compactions, 0u);
  std::printf("[ power loss ] %llu crash points, rollover=%d; uninterrupted run: %llu durability "
              "operations, %llu flushes, %llu compactions\n",
              static_cast<unsigned long long>(crash_points), manifest_rollover ? 1 : 0,
              static_cast<unsigned long long>(full.operations),
              static_cast<unsigned long long>(full.flushes),
              static_cast<unsigned long long>(full.compactions));
}

TEST(PowerLossTest, EveryCrashPointKeepsAcknowledgedDurableWrites) {
  SweepCrashPoints(/*num_ops=*/150, /*seed=*/1, /*manifest_rollover=*/false);
}

TEST(PowerLossTest, EveryCrashPointDuringManifestRolloversKeepsAcknowledgedDurableWrites) {
  SweepCrashPoints(/*num_ops=*/150, /*seed=*/2, /*manifest_rollover=*/true);
}

// The model itself: unsynced data and undurable directory changes vanish,
// synced ones stay.
TEST(PowerLossEnvTest, KeepsOnlyWhatWasSynced) {
  test::TempDir dir;
  const std::string db = dir.Sub("db");
  const std::string scratch = dir.Sub("scratch");
  ASSERT_EQ(0, ::mkdir(db.c_str(), 0755));
  ASSERT_EQ(0, ::mkdir(scratch.c_str(), 0755));
  test::WriteFileBytes(db + "/old", "durable");
  test::PowerLossEnv env(db, scratch);

  std::unique_ptr<WritableFile> synced;
  ASSERT_OK(env.NewWritableFile(db + "/synced", &synced));
  ASSERT_OK(synced->Append("abc"));
  ASSERT_OK(synced->Sync());
  ASSERT_OK(synced->Append("def"));  // Never synced.
  ASSERT_OK(synced->Close());
  ASSERT_OK(env.SyncDir(db));

  std::unique_ptr<WritableFile> unlinked_entry;
  ASSERT_OK(env.NewWritableFile(db + "/no-dir-sync", &unlinked_entry));
  ASSERT_OK(unlinked_entry->Append("x"));
  ASSERT_OK(unlinked_entry->Sync());
  ASSERT_OK(unlinked_entry->Close());

  ASSERT_OK(env.RenameFile(db + "/synced", db + "/renamed"));  // Not dir-synced.
  ASSERT_OK(env.RemoveFile(db + "/old"));                       // Not dir-synced.

  env.SimulatePowerLoss();
  EXPECT_EQ((std::vector<std::string>{"old", "synced"}), test::ListDir(db));
  EXPECT_EQ("abc", test::ReadFileBytes(db + "/synced"));
  EXPECT_EQ("durable", test::ReadFileBytes(db + "/old"));
}

}  // namespace
}  // namespace kvlog
