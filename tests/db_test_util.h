#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "db/db_impl.h"
#include "kvlog/db.h"
#include "test_util.h"

namespace kvlog::test {

// A database in a fresh temporary directory, plus shortcuts for the
// operations most database tests need.
class DBTestBase : public testing::Test {
 protected:
  DBTestBase() : dbname_(dir_.Sub("db")) { options_.create_if_missing = true; }

  void SetUp() override { ASSERT_OK(Reopen()); }

  // Closes the database before any member of a derived fixture (an Env it
  // uses, say) is destroyed.
  void TearDown() override { Close(); }

  // For tests that flush many times; see NoSyncEnv. Takes effect at the
  // next Reopen().
  void UseNoSyncEnv() { options_.env = &no_sync_env_; }

  // Closes the database (if open) and opens it again with options_.
  Status Reopen() {
    db_.reset();
    return DB::Open(options_, dbname_, &db_);
  }

  void Close() { db_.reset(); }

  DBImpl* impl() const { return static_cast<DBImpl*>(db_.get()); }

  Status Put(std::string_view key, std::string_view value, bool sync = false) {
    WriteOptions write_options;
    write_options.sync = sync;
    return db_->Put(write_options, key, value);
  }
  Status Delete(std::string_view key) { return db_->Delete(WriteOptions(), key); }
  std::string Get(std::string_view key) { return GetOrStatus(db_.get(), key); }
  std::map<std::string, std::string> Scan() { return ScanAll(db_.get()); }

  // Flushes and waits for the cleanup after it, so that file listings are
  // deterministic.
  Status FlushAndSettle() {
    Status s = db_->Flush();
    impl()->TEST_WaitForBackgroundIdle();
    return s;
  }

  int FilesAtLevel(int level) {
    std::string value;
    EXPECT_TRUE(db_->GetProperty("kvlog.num-files-at-level" + std::to_string(level), &value));
    return std::stoi(value);
  }

  // Checks that the open database did not need point-in-time recovery (it
  // reports that in kvlog.stats as a "recovery:" line).
  void ExpectNoRecoveryNote() {
    std::string stats;
    ASSERT_TRUE(db_->GetProperty("kvlog.stats", &stats));
    EXPECT_EQ(std::string::npos, stats.find("recovery:")) << stats;
  }

  // Full paths of the files in the database directory ending in `suffix`.
  std::vector<std::string> DbFiles(std::string_view suffix) const {
    return FilesWithSuffix(dbname_, suffix);
  }

  TempDir dir_;
  const std::string dbname_;
  NoSyncEnv no_sync_env_;  // Declared before db_ so it outlives the database.
  Options options_;
  std::unique_ptr<DB> db_;
};

}  // namespace kvlog::test
