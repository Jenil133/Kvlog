// I/O failures on every kind of file the database writes. A failure must
// reach the caller, must never expose a half-written table, must not let
// later writes land after a possibly torn log record, and must leave every
// acknowledged write recoverable by a reopen.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "db_test_util.h"
#include "fault_env.h"
#include "kvlog/write_batch.h"
#include "test_util.h"

namespace kvlog {
namespace {

using test::FileKind;
using test::Key;

class FaultInjectionTest : public test::DBTestBase {
 protected:
  void SetUp() override {
    options_.env = &env_;
    ASSERT_OK(Reopen());
  }

  test::FaultInjectionEnv env_;  // TearDown closes the database first.
};

TEST_F(FaultInjectionTest, FailedLogAppendFailsTheWriteAndStopsLaterWrites) {
  ASSERT_OK(Put("before", "1"));
  env_.FailAppends(FileKind::kLog, true);
  const Status s = Put("failed", "2");
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  env_.FailAppends(FileKind::kLog, false);

  // The log may now end in a partial record. Anything appended after it
  // would be lost on recovery, so writes are refused until a reopen.
  EXPECT_TRUE(Put("after", "3").IsIOError());
  EXPECT_TRUE(Delete("before").IsIOError());
  EXPECT_TRUE(db_->Flush().IsIOError());
  EXPECT_TRUE(impl()->TEST_BackgroundError().IsIOError());
  EXPECT_EQ("1", Get("before"));  // Reads keep working.
  EXPECT_EQ("NOT_FOUND", Get("failed"));

  ASSERT_OK(Reopen());
  EXPECT_EQ("1", Get("before"));
  EXPECT_EQ("NOT_FOUND", Get("failed"));
  EXPECT_EQ("NOT_FOUND", Get("after"));
  ASSERT_OK(Put("after", "3"));
}

TEST_F(FaultInjectionTest, FailedLogSyncFailsTheSyncWrite) {
  ASSERT_OK(Put("before", "1", /*sync=*/true));
  env_.FailSyncs(FileKind::kLog, true);
  EXPECT_TRUE(Put("unsynced", "2", /*sync=*/true).IsIOError());
  env_.FailSyncs(FileKind::kLog, false);
  EXPECT_TRUE(Put("after", "3").IsIOError());

  ASSERT_OK(Reopen());
  EXPECT_EQ("1", Get("before"));
  // The record reached the operating system before the sync failed, so its
  // fate is unknown to the caller; either outcome is consistent.
  const std::string unsynced = Get("unsynced");
  EXPECT_TRUE(unsynced == "2" || unsynced == "NOT_FOUND") << unsynced;
  EXPECT_EQ("NOT_FOUND", Get("after"));
}

TEST_F(FaultInjectionTest, OnlySyncWritesSyncTheLog) {
  const int base = env_.syncs(FileKind::kLog);
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Delete("a"));
  EXPECT_EQ(base, env_.syncs(FileKind::kLog));
  ASSERT_OK(Put("b", "2", /*sync=*/true));
  EXPECT_EQ(base + 1, env_.syncs(FileKind::kLog));
  WriteBatch batch;
  batch.Put("c", "3");
  batch.Delete("b");
  WriteOptions sync_options;
  sync_options.sync = true;
  ASSERT_OK(db_->Write(sync_options, batch));
  EXPECT_EQ(base + 2, env_.syncs(FileKind::kLog));
}

// The table is written under a temporary name and synced, renamed into
// place, and the rename made durable with a directory sync, all before the
// MANIFEST edit that refers to it is written and synced. The log being
// retired is synced first, so a newer log is never durable while an older
// one is not.
TEST_F(FaultInjectionTest, FlushFollowsTheDurableCommitProtocol) {
  ASSERT_OK(Put("k", "v"));
  const int log_syncs = env_.syncs(FileKind::kLog);
  const int table_syncs = env_.syncs(FileKind::kTemp);
  const int manifest_syncs = env_.syncs(FileKind::kManifest);
  const int directory_syncs = env_.directory_syncs();
  const int renames = env_.renames();

  env_.StartTrace();
  ASSERT_OK(FlushAndSettle());
  // The order is what makes a crash at any point safe: the retiring log is
  // durable before a newer one takes writes; the new log's entry is durable
  // before a sync write to it is acknowledged; the table's data and name
  // are durable before the MANIFEST refers to it; and the old log goes only
  // after the MANIFEST edit that makes it obsolete is durable.
  EXPECT_EQ((std::vector<std::string>{"sync log", "create log", "syncdir", "create tmp",
                                      "sync tmp", "rename tmp->sst", "syncdir", "sync manifest",
                                      "remove log"}),
            env_.TakeTrace());
  EXPECT_EQ(log_syncs + 1, env_.syncs(FileKind::kLog));
  EXPECT_EQ(table_syncs + 1, env_.syncs(FileKind::kTemp));
  EXPECT_EQ(renames + 1, env_.renames());
  // One for the new log's directory entry, one for the table's rename.
  EXPECT_EQ(directory_syncs + 2, env_.directory_syncs());
  EXPECT_EQ(manifest_syncs + 1, env_.syncs(FileKind::kManifest));
  EXPECT_EQ(1u, DbFiles(".sst").size());
  EXPECT_TRUE(DbFiles(".tmp").empty());
}

// Point-in-time recovery sets the damaged log aside durably (copy synced,
// note synced, lost/ and its entry synced) before the MANIFEST edit that
// makes the original obsolete, and deletes the original only after it. A
// crash anywhere in the open therefore leaves the log in one place or the
// other.
TEST_F(FaultInjectionTest, RecoverySetsLogsAsideBeforeRetiringThem) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "2"));
  ASSERT_OK(Put("c", "3"));
  Close();
  const std::vector<std::string> logs = DbFiles(".log");
  ASSERT_EQ(1u, logs.size());
  // Three records of equal size: the middle of the file is inside the second.
  test::CorruptByte(logs[0], test::FileSize(logs[0]) / 2);

  env_.StartTrace();
  ASSERT_OK(Reopen());
  const std::vector<std::string> trace = env_.TakeTrace();
  // The copy and the note, each synced, then lost/ and the database directory.
  const std::vector<std::string> set_aside = {"create log", "sync log",  "create other",
                                              "sync other", "syncdir",   "syncdir"};
  const auto copied = std::search(trace.begin(), trace.end(), set_aside.begin(), set_aside.end());
  const auto manifest_sync = std::find(trace.begin(), trace.end(), "sync manifest");
  const auto removal = std::find(trace.begin(), trace.end(), "remove log");
  EXPECT_TRUE(copied != trace.end() && manifest_sync != trace.end() && removal != trace.end() &&
              copied < manifest_sync && manifest_sync < removal)
      << testing::PrintToString(trace);
  EXPECT_EQ("1", Get("a"));
  EXPECT_EQ("NOT_FOUND", Get("c"));
}

TEST_F(FaultInjectionTest, FailedTableWriteLeavesNoPartialTable) {
  for (uint64_t i = 0; i < 100; ++i) {
    ASSERT_OK(Put(Key(i), "v"));
  }
  env_.FailAppends(FileKind::kTemp, true);
  EXPECT_TRUE(db_->Flush().IsIOError());
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_TRUE(DbFiles(".sst").empty());
  EXPECT_TRUE(DbFiles(".tmp").empty());
  EXPECT_EQ("v", Get(Key(5)));  // Still served from the frozen memtable.
  EXPECT_TRUE(Put("x", "y").IsIOError());
  env_.FailAppends(FileKind::kTemp, false);

  ASSERT_OK(Reopen());  // The log was kept, so nothing is lost.
  for (uint64_t i = 0; i < 100; ++i) {
    EXPECT_EQ("v", Get(Key(i)));
  }
}

TEST_F(FaultInjectionTest, FailedTableCreateOrRenameStopsWritesUntilReopen) {
  for (const bool fail_rename : {false, true}) {
    const std::string value = fail_rename ? "second" : "first";
    ASSERT_OK(Put("k", value));
    if (fail_rename) {
      env_.FailRenames(true);
    } else {
      env_.FailCreates(FileKind::kTemp, true);
    }
    EXPECT_TRUE(db_->Flush().IsIOError());
    impl()->TEST_WaitForBackgroundIdle();
    EXPECT_TRUE(DbFiles(".tmp").empty());
    EXPECT_TRUE(Put("x", "y").IsIOError());
    env_.FailRenames(false);
    env_.FailCreates(FileKind::kTemp, false);

    ASSERT_OK(Reopen());
    EXPECT_EQ(value, Get("k"));
    EXPECT_EQ("NOT_FOUND", Get("x"));
  }
}

TEST_F(FaultInjectionTest, FailedManifestWriteLeavesTheDataInTheLog) {
  for (uint64_t i = 0; i < 100; ++i) {
    ASSERT_OK(Put(Key(i), "v"));
  }
  env_.FailAppends(FileKind::kManifest, true);
  EXPECT_TRUE(db_->Flush().IsIOError());
  impl()->TEST_WaitForBackgroundIdle();
  env_.FailAppends(FileKind::kManifest, false);

  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 100; ++i) {
    EXPECT_EQ("v", Get(Key(i)));
  }
  // The table the failed update would have added was never committed;
  // recovery deleted it and wrote the replayed log to a new one.
  EXPECT_EQ(1u, DbFiles(".sst").size());
  EXPECT_EQ(1, FilesAtLevel(0));
}

// Starting a new log fails before the memtable is frozen, so only that
// flush request fails; the database stays usable.
TEST_F(FaultInjectionTest, FailedSwitchToANewLogFailsOnlyThatFlush) {
  ASSERT_OK(Put("k", "v"));
  env_.FailDirectorySyncs(true);
  EXPECT_TRUE(db_->Flush().IsIOError());
  env_.FailDirectorySyncs(false);
  EXPECT_OK(impl()->TEST_BackgroundError());
  EXPECT_EQ(1u, DbFiles(".log").size());  // The new log was removed again.

  ASSERT_OK(Put("k2", "v2"));
  ASSERT_OK(FlushAndSettle());
  EXPECT_EQ(1, FilesAtLevel(0));
  ASSERT_OK(Reopen());
  EXPECT_EQ("v", Get("k"));
  EXPECT_EQ("v2", Get("k2"));
}

// With a MANIFEST rollover on every update, each flush ends by pointing
// CURRENT at a new MANIFEST: write a temporary file, sync it, rename it over
// CURRENT, sync the directory. A failure at any of those steps must leave a
// database that opens with every acknowledged write. After the rename, the
// new MANIFEST may be the live one and must not be deleted.
TEST_F(FaultInjectionTest, FailedCurrentUpdateDuringARolloverLosesNothing) {
  // In a flush, the table install comes first: one temporary-file sync,
  // one rename and (after the memtable switch's) one directory sync.
  const std::vector<std::pair<const char*, std::function<void()>>> faults = {
      {"CURRENT temp sync", [&] { env_.FailSyncsAfter(FileKind::kTemp, 1); }},
      {"CURRENT rename", [&] { env_.FailRenamesAfter(1); }},
      {"directory sync after the rename", [&] { env_.FailDirectorySyncsAfter(2); }},
  };
  uint64_t next_key = 0;
  for (const auto& [what, inject] : faults) {
    options_.max_manifest_file_size = 1;
    ASSERT_OK(Reopen()) << what;
    const uint64_t begin = next_key;
    for (; next_key < begin + 50; ++next_key) {
      ASSERT_OK(Put(Key(next_key), "v", /*sync=*/next_key % 2 == 0)) << what;
    }
    inject();
    EXPECT_TRUE(db_->Flush().IsIOError()) << what;
    impl()->TEST_WaitForBackgroundIdle();
    env_.ClearFailures();
    EXPECT_TRUE(impl()->TEST_BackgroundError().IsIOError()) << what;

    ASSERT_OK(Reopen()) << what;
    for (uint64_t i = 0; i < next_key; ++i) {
      ASSERT_EQ("v", Get(Key(i))) << what << ", key " << i;
    }
    // And the database keeps working across further updates and reopens.
    ASSERT_OK(Put("after", what));
    ASSERT_OK(FlushAndSettle()) << what;
    ASSERT_OK(Reopen()) << what;
    EXPECT_EQ(what, Get("after"));
  }
}

TEST_F(FaultInjectionTest, OpenFailsCleanlyWhenItCannotStartALogOrManifest) {
  ASSERT_OK(Put("k", "v"));
  for (const FileKind kind : {FileKind::kLog, FileKind::kManifest}) {
    Close();
    env_.FailCreates(kind, true);
    EXPECT_TRUE(Reopen().IsIOError());
    env_.FailCreates(kind, false);
    // The failed open released the lock and lost nothing.
    ASSERT_OK(Reopen());
    EXPECT_EQ("v", Get("k"));
  }
}

}  // namespace
}  // namespace kvlog
