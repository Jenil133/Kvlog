// Recovery after crashes and damage: unclean shutdowns (an image of an open
// database, a process killed mid-write), logs cut or damaged anywhere, an
// interrupted MANIFEST update, and the files a crash leaves behind.

#include <signal.h>
#include <sys/wait.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crash_ops.h"
#include "crash_test_util.h"
#include "db/log_format.h"
#include "db/log_writer.h"
#include "db/write_batch_internal.h"
#include "db_test_util.h"
#include "fault_env.h"
#include "kvlog/write_batch.h"
#include "subprocess.h"
#include "test_util.h"
#include "util/coding.h"

namespace kvlog {
namespace {

using test::Key;

class RecoveryTest : public test::DBTestBase {
 protected:
  // The database's only log file.
  std::string OnlyLog() const {
    const std::vector<std::string> logs = DbFiles(".log");
    EXPECT_EQ(1u, logs.size());
    return logs.empty() ? std::string() : logs.front();
  }

  // Size of the log record holding `batch`.
  static uint64_t RecordSize(const WriteBatch& batch) {
    return log::kHeaderSize + WriteBatchInternal::ByteSize(batch);
  }

  // Runs the crash helper to completion and returns its wait status.
  static int RunHelper(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {KVLOG_CRASH_HELPER_PATH};
    argv.insert(argv.end(), args.begin(), args.end());
    test::Subprocess child;
    EXPECT_TRUE(child.Start(argv));
    std::string line;
    while (child.ReadLine(&line)) {
    }
    return child.Wait();
  }

  // For the tests that inject failures. TearDown closes db_ before it dies.
  test::FaultInjectionEnv fault_env_;
};

TEST_F(RecoveryTest, UnflushedWritesAreReplayedFromTheLog) {
  std::map<std::string, std::string> model;
  for (uint64_t i = 0; i < 500; ++i) {
    ASSERT_OK(Put(Key(i), "v" + std::to_string(i)));
    model[Key(i)] = "v" + std::to_string(i);
  }
  for (uint64_t i = 0; i < 500; i += 3) {
    ASSERT_OK(Delete(Key(i)));
    model.erase(Key(i));
  }
  Close();
  // Closing does not flush: everything is still only in the log.
  EXPECT_TRUE(DbFiles(".sst").empty());
  ASSERT_OK(Reopen());
  EXPECT_EQ(model, Scan());
  // Recovery wrote the replayed updates to a table and retired the log.
  EXPECT_EQ(1, FilesAtLevel(0));
  EXPECT_EQ(1u, DbFiles(".log").size());
  EXPECT_NE(OnlyLog(), "");
  std::string stats;
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
  EXPECT_NE(std::string::npos, stats.find("batches recovered: 667")) << stats;  // 500 + 167.
  EXPECT_NE(std::string::npos, stats.find("flushes: 1 ")) << stats;
}

// A copy of the directory of a database that is still open (never flushed,
// never closed) is exactly what a crash at that moment leaves on disk.
TEST_F(RecoveryTest, ImageOfAnOpenDatabaseRecoversEveryAcknowledgedWrite) {
  std::map<std::string, std::string> model;
  for (uint64_t i = 0; i < 300; ++i) {
    const std::string value = "v" + std::to_string(i);
    ASSERT_OK(Put(Key(i % 200), value, /*sync=*/i % 100 == 0));
    model[Key(i % 200)] = value;
    if (i == 150) {
      ASSERT_OK(db_->Flush());  // Some updates in a table, the rest in the log.
    }
  }
  ASSERT_OK(Delete(Key(7)));
  model.erase(Key(7));
  impl()->TEST_WaitForBackgroundIdle();  // No files appear or vanish mid-copy.

  const std::string image = dir_.Sub("image");
  test::CopyDir(dbname_, image);
  std::unique_ptr<DB> recovered;
  ASSERT_OK(DB::Open(options_, image, &recovered));
  EXPECT_EQ(model, test::ScanAll(recovered.get()));
  EXPECT_EQ(model, Scan());  // The original is unaffected.
}

TEST_F(RecoveryTest, ImageWithAFlushPendingReplaysBothLogsInOrder) {
  impl()->TEST_PauseBackgroundWork();
  for (uint64_t i = 0; i < 100; ++i) {
    ASSERT_OK(Put(Key(i), "old"));
  }
  ASSERT_OK(impl()->TEST_SwitchMemTable());  // Frozen, but never flushed.
  for (uint64_t i = 0; i < 100; i += 2) {
    ASSERT_OK(Put(Key(i), "new"));
  }
  for (uint64_t i = 1; i < 100; i += 4) {
    ASSERT_OK(Delete(Key(i)));
  }
  EXPECT_EQ(2u, DbFiles(".log").size());
  const std::string image = dir_.Sub("image");
  test::CopyDir(dbname_, image);
  impl()->TEST_ResumeBackgroundWork();

  std::unique_ptr<DB> recovered;
  ASSERT_OK(DB::Open(options_, image, &recovered));
  for (uint64_t i = 0; i < 100; ++i) {
    const char* expected = i % 2 == 0 ? "new" : (i % 4 == 1 ? "NOT_FOUND" : "old");
    EXPECT_EQ(expected, test::GetOrStatus(recovered.get(), Key(i))) << Key(i);
  }
}

// The real thing: a process that is killed (no flush, no close, no
// destructors) while its background thread is flushing.
TEST_F(RecoveryTest, KilledProcessLosesNoAcknowledgedWrite) {
  Close();  // The helper owns the database now.
  const int status =
      RunHelper({"--skip-fs-sync", "write-then-die", dbname_, "3000", "16384", "0"});
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) << "wait status " << status;
  ASSERT_OK(Reopen());
  EXPECT_EQ(3000u, Scan().size());
  for (uint64_t i = 0; i < 3000; ++i) {
    ASSERT_EQ(test::CrashOpStream::ValueFor(i, 100), Get(test::CrashOpStream::KeyFor(i)));
  }
}

TEST_F(RecoveryTest, KilledProcessKeepsSyncedWrites) {
  Close();
  const int status = RunHelper({"write-then-die", dbname_, "20", "4194304", "1"});
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) << "wait status " << status;
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 20; ++i) {
    EXPECT_EQ(test::CrashOpStream::ValueFor(i, 100), Get(test::CrashOpStream::KeyFor(i)));
  }
}

TEST_F(RecoveryTest, KilledDuringRandomWritesKeepsEveryAcknowledgedOp) {
  Close();
  const std::string ack_file = dir_.Sub("acks");
  test::CrashRun run;
  run.seed = 20241008;
  run.kill_after_acks = 1500;
  test::RunRandomWritesAndKill(dbname_, ack_file, run);
  ASSERT_OK(Reopen());
  EXPECT_GE(test::VerifyAgainstAcks(db_.get(), run.seed, ack_file), run.kill_after_acks);
}

TEST_F(RecoveryTest, KilledDuringSyncedRandomWritesKeepsEveryAcknowledgedOp) {
  Close();
  const std::string ack_file = dir_.Sub("acks");
  test::CrashRun run;
  run.seed = 7;
  run.sync = true;
  run.skip_fs_sync = false;
  run.kill_after_acks = 20;
  test::RunRandomWritesAndKill(dbname_, ack_file, run);
  ASSERT_OK(Reopen());
  EXPECT_GE(test::VerifyAgainstAcks(db_.get(), run.seed, ack_file), run.kill_after_acks);
}

// A crash can cut the log anywhere in its last record. Every cut must
// recover exactly the writes before that record, and the record's batch
// all or nothing, and a torn tail is not damage even for a paranoid open.
TEST_F(RecoveryTest, LogCutAnywhereInTheLastRecordKeepsExactlyTheCompleteWrites) {
  UseNoSyncEnv();  // One recovery per cut point.
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 10; ++i) {
    ASSERT_OK(Put(Key(i), "v" + std::to_string(i)));
  }
  WriteBatch last;
  last.Put("batch-a", "1");
  last.Delete(Key(3));
  last.Put("batch-b", std::string(100, 'b'));
  ASSERT_OK(db_->Write(WriteOptions(), last));
  Close();

  const std::string log = OnlyLog();
  const std::string log_name = log.substr(dbname_.size() + 1);
  const uint64_t size = test::FileSize(log);
  const uint64_t last_start = size - RecordSize(last);
  for (uint64_t cut = last_start; cut <= size; ++cut) {
    const bool complete = cut == size;
    for (const bool paranoid : {false, true}) {
      const std::string image = dir_.Sub("image");
      test::CopyDir(dbname_, image);
      test::TruncateFile(image + "/" + log_name, cut);
      Options options = options_;
      options.paranoid_checks = paranoid;
      std::unique_ptr<DB> db;
      ASSERT_OK(DB::Open(options, image, &db)) << "cut at " << cut;
      for (uint64_t i = 0; i < 10; ++i) {
        const std::string expected = (i == 3 && complete) ? "NOT_FOUND" : "v" + std::to_string(i);
        EXPECT_EQ(expected, test::GetOrStatus(db.get(), Key(i))) << "cut at " << cut;
      }
      EXPECT_EQ(complete ? "1" : "NOT_FOUND", test::GetOrStatus(db.get(), "batch-a"))
          << "cut at " << cut;
      EXPECT_EQ(complete ? std::string(100, 'b') : "NOT_FOUND",
                test::GetOrStatus(db.get(), "batch-b"))
          << "cut at " << cut;
      db.reset();
      test::RemoveAll(image);
    }
  }
}

TEST_F(RecoveryTest, DamagedLogRecordFailsAParanoidOpenOrEndsReplayThere) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(Put("c", "3"));
  Close();
  const std::string log = OnlyLog();
  WriteBatch probe;
  probe.Put("a", "1");
  const uint64_t record_size = RecordSize(probe);
  ASSERT_EQ(3 * record_size, test::FileSize(log));
  test::CorruptByte(log, record_size + log::kHeaderSize + 14);  // In "b"'s payload.
  const std::string damaged = test::ReadFileBytes(log);

  options_.paranoid_checks = true;
  const Status s = Reopen();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  // A failed open changes nothing on disk.
  EXPECT_EQ(damaged, test::ReadFileBytes(log));
  EXPECT_TRUE(DbFiles(".sst").empty());

  // Otherwise recovery keeps the writes before the damage and none after:
  // replaying "c" without "b" would invent a state that never existed.
  options_.paranoid_checks = false;
  ASSERT_OK(Reopen());
  EXPECT_EQ("1", Get("a"));
  EXPECT_EQ("NOT_FOUND", Get("b"));
  EXPECT_EQ("NOT_FOUND", Get("c"));
  std::string stats;
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
  EXPECT_NE(std::string::npos, stats.find("recovery: replay stopped")) << stats;
  // The damaged log holds acknowledged writes ("c") that might still be
  // salvaged by hand, so it is set aside with a note instead of deleted.
  const std::string lost = dbname_ + "/lost";
  const std::vector<std::string> lost_files = test::ListDir(lost);
  ASSERT_EQ(2u, lost_files.size());
  EXPECT_EQ(log.substr(dbname_.size() + 1), lost_files[0]);
  EXPECT_EQ(damaged, test::ReadFileBytes(lost + "/" + lost_files[0]));
  EXPECT_TRUE(lost_files[1].starts_with("RECOVERY-")) << lost_files[1];
  EXPECT_TRUE(DbFiles(".log").size() == 1 && DbFiles(".log")[0] != log);

  ASSERT_OK(Put("d", "4"));  // Usable again, and the damage is gone for good.
  ASSERT_OK(Reopen());
  EXPECT_EQ("1", Get("a"));
  EXPECT_EQ("4", Get("d"));
}

// The logs point-in-time recovery stops in are copied to lost/ before the
// MANIFEST edit that retires them is committed. If they cannot be set aside,
// the open fails and leaves them where they are rather than letting them be
// deleted as obsolete.
TEST_F(RecoveryTest, LogsThatCannotBeSetAsideAreNotDropped) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(Put("c", "3"));
  Close();
  const std::string log = OnlyLog();
  WriteBatch probe;
  probe.Put("a", "1");
  test::CorruptByte(log, RecordSize(probe) + log::kHeaderSize + 14);  // In "b"'s payload.
  const std::string damaged = test::ReadFileBytes(log);
  const std::vector<std::string> before = test::ListDir(dbname_);

  // A plain file where the lost/ directory should go makes every copy fail.
  const std::string lost = dbname_ + "/lost";
  test::WriteFileBytes(lost, "in the way");
  const Status s = Reopen();
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(damaged, test::ReadFileBytes(log));
  EXPECT_EQ(before.size() + 1, test::ListDir(dbname_).size());  // Only the plain file added.
  EXPECT_TRUE(DbFiles(".sst").empty());

  test::RemoveFile(lost);
  ASSERT_OK(Reopen());
  EXPECT_EQ("1", Get("a"));
  EXPECT_EQ("NOT_FOUND", Get("b"));
  EXPECT_EQ(damaged, test::ReadFileBytes(lost + "/" + log.substr(dbname_.size() + 1)));
}

// Replay writes level-0 tables as it goes. If it then finds damage and the
// paranoid open fails, those tables must not be left behind.
TEST_F(RecoveryTest, FailedParanoidOpenLeavesTheDirectoryAsItWas) {
  UseNoSyncEnv();
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 2000; ++i) {
    ASSERT_OK(Put(Key(i), std::string(100, 'v')));
  }
  Close();
  const std::string log = OnlyLog();
  test::CorruptByte(log, test::FileSize(log) - 200);  // Inside the second to last record.
  const std::vector<std::string> before = test::ListDir(dbname_);
  const std::string log_bytes = test::ReadFileBytes(log);

  options_.paranoid_checks = true;
  options_.write_buffer_size = 4096;  // Replay flushes many tables before the damage.
  const Status s = Reopen();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
  EXPECT_EQ(before, test::ListDir(dbname_));
  EXPECT_EQ(log_bytes, test::ReadFileBytes(log));
}

TEST_F(RecoveryTest, DamageInAnOlderLogDropsEverythingAfterIt) {
  impl()->TEST_PauseBackgroundWork();
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(Put("c", "3"));
  ASSERT_OK(impl()->TEST_SwitchMemTable());
  ASSERT_OK(Put("d", "4"));  // These two are in the newer log.
  ASSERT_OK(Put("a", "5"));
  const std::string image = dir_.Sub("image");
  test::CopyDir(dbname_, image);
  impl()->TEST_ResumeBackgroundWork();

  const std::vector<std::string> logs = test::FilesWithSuffix(image, ".log");
  ASSERT_EQ(2u, logs.size());  // Zero-padded names: the older log sorts first.
  WriteBatch probe;
  probe.Put("a", "1");
  test::CorruptByte(logs[0], RecordSize(probe) + log::kHeaderSize + 14);  // "b"

  std::unique_ptr<DB> recovered;
  ASSERT_OK(DB::Open(options_, image, &recovered));
  EXPECT_EQ("1", test::GetOrStatus(recovered.get(), "a"));  // Not "5" from across the gap.
  EXPECT_EQ("NOT_FOUND", test::GetOrStatus(recovered.get(), "b"));
  EXPECT_EQ("NOT_FOUND", test::GetOrStatus(recovered.get(), "c"));
  EXPECT_EQ("NOT_FOUND", test::GetOrStatus(recovered.get(), "d"));
}

// A torn tail is what an interrupted append leaves, but every log is synced
// before the next one takes writes. A torn older log followed by a newer
// log with data means a sync never reached the disk; replaying the newer
// log would apply later writes without earlier ones.
TEST_F(RecoveryTest, TornTailInAnOlderLogFollowedByDataIsDamage) {
  impl()->TEST_PauseBackgroundWork();
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(impl()->TEST_SwitchMemTable());
  ASSERT_OK(Put("c", "3"));  // In the newer log.
  const std::string image = dir_.Sub("image");
  test::CopyDir(dbname_, image);
  impl()->TEST_ResumeBackgroundWork();
  const std::vector<std::string> logs = test::FilesWithSuffix(image, ".log");
  ASSERT_EQ(2u, logs.size());
  test::TruncateFile(logs[0], test::FileSize(logs[0]) - 3);  // "b" is torn.

  Options options = options_;
  options.paranoid_checks = true;
  std::unique_ptr<DB> recovered;
  EXPECT_TRUE(DB::Open(options, image, &recovered).IsCorruption());
  options.paranoid_checks = false;
  ASSERT_OK(DB::Open(options, image, &recovered));
  EXPECT_EQ("1", test::GetOrStatus(recovered.get(), "a"));
  EXPECT_EQ("NOT_FOUND", test::GetOrStatus(recovered.get(), "b"));
  EXPECT_EQ("NOT_FOUND", test::GetOrStatus(recovered.get(), "c"));
  recovered.reset();
  // Both logs were set aside, not deleted.
  EXPECT_EQ(3u, test::ListDir(image + "/lost").size());
}

// An empty newer log (a crash during an Open that had created it) is no
// evidence of a lost sync: the torn tail is dropped as usual.

TEST_F(RecoveryTest, TornOlderLogFollowedByAnEmptyLogIsATornTail) {
  Close();
  options_.paranoid_checks = true;
  ASSERT_OK(Reopen());
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  const std::string image = dir_.Sub("image");
  test::CopyDir(dbname_, image);
  const std::vector<std::string> logs = test::FilesWithSuffix(image, ".log");
  ASSERT_EQ(1u, logs.size());
  test::TruncateFile(logs[0], test::FileSize(logs[0]) - 3);
  // What a crash inside Open leaves: a new, empty log numbered above it.
  test::WriteFileBytes(image + "/999999.log", "");
  std::unique_ptr<DB> recovered;
  ASSERT_OK(DB::Open(options_, image, &recovered));
  EXPECT_EQ("1", test::GetOrStatus(recovered.get(), "a"));
  EXPECT_EQ("NOT_FOUND", test::GetOrStatus(recovered.get(), "b"));
}

// A complete final log record with a flipped bit is not a torn append; it
// may be an acknowledged synced write. A paranoid open refuses; otherwise
// replay stops before it and the log is kept in lost/.
TEST_F(RecoveryTest, DamagedFinalLogRecordIsDamageNotATornTail) {
  ASSERT_OK(Put("a", "1", /*sync=*/true));
  ASSERT_OK(Put("b", "2", /*sync=*/true));
  Close();
  const std::string log = OnlyLog();
  test::CorruptByte(log, test::FileSize(log) - 1, 0x01);
  options_.paranoid_checks = true;
  EXPECT_TRUE(Reopen().IsCorruption());
  options_.paranoid_checks = false;
  ASSERT_OK(Reopen());
  EXPECT_EQ("1", Get("a"));
  EXPECT_EQ("NOT_FOUND", Get("b"));
  EXPECT_EQ(2u, test::ListDir(dbname_ + "/lost").size());
}

// The probe from a review: a committed MANIFEST edit damaged at rest used
// to be dropped as a torn tail, after which Open deleted the table it had
// added as an orphan. Now the open fails and nothing is deleted.
TEST_F(RecoveryTest, DamagedFinalManifestEditFailsTheOpenAndDeletesNothing) {
  for (uint64_t i = 0; i < 100; ++i) {
    ASSERT_OK(Put(Key(i), "v"));
  }
  ASSERT_OK(FlushAndSettle());
  Close();
  const std::vector<std::string> manifests = DbFiles("");
  std::string manifest;
  for (const std::string& path : manifests) {
    if (path.find("MANIFEST-") != std::string::npos) {
      manifest = path;
    }
  }
  ASSERT_FALSE(manifest.empty());
  test::CorruptByte(manifest, test::FileSize(manifest) - 2, 0x01);
  const std::vector<std::string> before = test::ListDir(dbname_);
  for (const bool paranoid : {true, false}) {
    options_.paranoid_checks = paranoid;
    const Status s = Reopen();
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    EXPECT_EQ(before, test::ListDir(dbname_));
  }
}

TEST_F(RecoveryTest, CreateIfMissingReportsWhyTheDirectoryCouldNotBeCreated) {
  Close();
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options_, dir_.Sub("missing-parent") + "/db", &db);
  EXPECT_FALSE(s.ok());
  EXPECT_FALSE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_NE(std::string::npos, s.ToString().find("missing-parent")) << s.ToString();
}

// A record can pass both checksums and still not hold a valid batch (a
// foreign or buggy writer). That is damage, handled like a bad checksum.
TEST_F(RecoveryTest, MalformedBatchInAnIntactRecordIsDamage) {
  ASSERT_OK(Put("a", "1"));
  Close();
  const std::string log = OnlyLog();
  const std::string original = test::ReadFileBytes(log);

  WriteBatch miscounted;
  miscounted.Put("x", "y");
  std::string miscounted_contents(WriteBatchInternal::Contents(miscounted));
  EncodeFixed32(miscounted_contents.data() + 8, 5);  // Claims five operations.
  WriteBatch good;
  good.Put("z", "after the damage");
  WriteBatchInternal::SetSequence(&good, 100);

  for (const std::string& bad : {miscounted_contents, std::string("short")}) {
    std::string appended;
    log::Writer writer(std::make_unique<test::StringSink>(&appended));
    ASSERT_OK(writer.AddRecord(bad));
    ASSERT_OK(writer.AddRecord(WriteBatchInternal::Contents(good)));

    const std::string image = dir_.Sub("image");
    test::CopyDir(dbname_, image);
    test::WriteFileBytes(image + log.substr(dbname_.size()), original + appended);
    Options options = options_;
    options.paranoid_checks = true;
    std::unique_ptr<DB> db;
    const Status s = DB::Open(options, image, &db);
    EXPECT_TRUE(s.IsCorruption()) << s.ToString();
    options.paranoid_checks = false;
    ASSERT_OK(DB::Open(options, image, &db));
    EXPECT_EQ("1", test::GetOrStatus(db.get(), "a"));
    EXPECT_EQ("NOT_FOUND", test::GetOrStatus(db.get(), "x"));
    EXPECT_EQ("NOT_FOUND", test::GetOrStatus(db.get(), "z"));
    db.reset();
    test::RemoveAll(image);
  }
}

TEST_F(RecoveryTest, FilesLeftByAnInterruptedFlushAreRemoved) {
  ASSERT_OK(Put("k", "v"));
  ASSERT_OK(FlushAndSettle());
  Close();
  const std::vector<std::string> tables = DbFiles(".sst");
  ASSERT_EQ(1u, tables.size());
  // A table the MANIFEST never listed, and a half-written temporary file.
  test::WriteFileBytes(dbname_ + "/000900.sst", "never committed");
  test::WriteFileBytes(dbname_ + "/000901.tmp", "half written");

  ASSERT_OK(Reopen());
  EXPECT_EQ(tables, DbFiles(".sst"));
  EXPECT_TRUE(DbFiles(".tmp").empty());
  EXPECT_EQ("v", Get("k"));
  // Numbers seen on disk are never handed out again.
  EXPECT_GT(impl()->TEST_LogFileNumber(), 901u);
}

TEST_F(RecoveryTest, MissingTableFileIsCorruption) {
  ASSERT_OK(Put("k", "v"));
  ASSERT_OK(db_->Flush());
  Close();
  const std::vector<std::string> tables = DbFiles(".sst");
  ASSERT_EQ(1u, tables.size());
  test::RemoveFile(tables.front());
  const Status s = Reopen();
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

// A flush commits by appending an edit to the MANIFEST and syncing it; the
// old log is deleted only after that. Whether a crash leaves the edit on
// disk whole (the table has the data) or torn (the old log still has it),
// nothing is lost.
TEST_F(RecoveryTest, FlushInterruptedBeforeItsManifestEditIsDurable) {
  options_.env = &fault_env_;
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 100; ++i) {
    ASSERT_OK(Put(Key(i), "v"));
  }
  fault_env_.FailSyncs(test::FileKind::kManifest, true);
  EXPECT_TRUE(db_->Flush().IsIOError());
  EXPECT_TRUE(impl()->TEST_BackgroundError().IsIOError());
  EXPECT_FALSE(Put("late", "x").ok());  // Writes stop until a reopen.
  Close();
  fault_env_.FailSyncs(test::FileKind::kManifest, false);

  std::string manifest;
  for (const std::string& name : test::ListDir(dbname_)) {
    if (name.starts_with("MANIFEST-")) {
      ASSERT_TRUE(manifest.empty()) << "more than one MANIFEST";
      manifest = name;
    }
  }
  const std::string whole = dir_.Sub("whole");
  const std::string torn = dir_.Sub("torn");
  test::CopyDir(dbname_, whole);
  test::CopyDir(dbname_, torn);
  test::TruncateFile(torn + "/" + manifest, test::FileSize(torn + "/" + manifest) - 1);

  for (const std::string& image : {whole, torn}) {
    std::unique_ptr<DB> db;
    ASSERT_OK(DB::Open(Options(), image, &db)) << image;
    for (uint64_t i = 0; i < 100; ++i) {
      ASSERT_EQ("v", test::GetOrStatus(db.get(), Key(i))) << image;
    }
    EXPECT_EQ("NOT_FOUND", test::GetOrStatus(db.get(), "late"));
  }
}

TEST_F(RecoveryTest, RepeatedReopensDoNotAccumulateFiles) {
  ASSERT_OK(Put("k", "v"));
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK(Reopen());
  }
  EXPECT_EQ(1u, DbFiles(".log").size());
  EXPECT_EQ(1u, DbFiles(".sst").size());  // The first reopen moved "k" into a table.
  int manifests = 0;
  for (const std::string& name : test::ListDir(dbname_)) {
    manifests += name.starts_with("MANIFEST-") ? 1 : 0;
  }
  EXPECT_EQ(1, manifests);
  EXPECT_EQ("v", Get("k"));
}

TEST_F(RecoveryTest, LogLargerThanTheWriteBufferRecoversIntoSeveralTables) {
  UseNoSyncEnv();
  ASSERT_OK(Reopen());
  std::map<std::string, std::string> model;
  for (uint64_t i = 0; i < 2000; ++i) {
    const std::string value = std::to_string(i) + std::string(80, 'x');
    ASSERT_OK(Put(Key(i % 1500), value));
    model[Key(i % 1500)] = value;
  }
  Close();
  options_.write_buffer_size = 4096;  // Far smaller than the log.
  // Otherwise the compaction that may start right after Open could merge
  // the tables before they are counted.
  options_.disable_auto_compactions = true;
  ASSERT_OK(Reopen());
  EXPECT_GT(FilesAtLevel(0), 5);
  EXPECT_EQ(model, Scan());
}

TEST_F(RecoveryTest, DatabaseOpenInAnotherProcessIsBusy) {
  Close();
  for (const bool kill_holder : {false, true}) {
    test::Subprocess holder;
    ASSERT_TRUE(holder.Start({KVLOG_CRASH_HELPER_PATH, "hold-open", dbname_}));
    std::string line;
    ASSERT_TRUE(holder.ReadLine(&line));
    ASSERT_EQ("open", line);
    const Status s = Reopen();
    EXPECT_TRUE(s.IsBusy()) << s.ToString();
    if (kill_holder) {
      // The kernel drops the lock of a process that dies without closing.
      holder.Kill(SIGKILL);
      holder.Wait();
    } else {
      holder.CloseStdin();
      const int status = holder.Wait();
      EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "wait status " << status;
    }
    ASSERT_OK(Reopen());
    Close();
  }
}

}  // namespace
}  // namespace kvlog
