#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db_test_util.h"
#include "kvlog/db.h"
#include "kvlog/statistics.h"
#include "kvlog/write_batch.h"
#include "test_util.h"

namespace kvlog {
namespace {

using test::Key;

class DBTest : public test::DBTestBase {};

TEST_F(DBTest, PutGetDeleteAndOverwrite) {
  EXPECT_EQ("NOT_FOUND", Get("k"));
  ASSERT_OK(Put("k", "v1"));
  EXPECT_EQ("v1", Get("k"));
  ASSERT_OK(Put("k", "v2"));
  EXPECT_EQ("v2", Get("k"));
  ASSERT_OK(Delete("k"));
  EXPECT_EQ("NOT_FOUND", Get("k"));
  ASSERT_OK(Delete("k"));        // Deleting a missing key is fine.
  ASSERT_OK(Delete("never"));
  ASSERT_OK(Put("k", "v3"));     // A key can come back after a delete.
  EXPECT_EQ("v3", Get("k"));

  std::string value = "stale";
  EXPECT_TRUE(db_->Get(ReadOptions(), "never", &value).IsNotFound());
}

TEST_F(DBTest, EmptyKeysValuesAndBinaryData) {
  const std::string binary_key("\x00\xff\x00", 3);
  const std::string binary_value("\xff\x00\x01", 3);
  ASSERT_OK(Put("", "value of the empty key"));
  ASSERT_OK(Put("empty-value", ""));
  ASSERT_OK(Put(binary_key, binary_value));
  for (int round = 0; round < 3; ++round) {
    EXPECT_EQ("value of the empty key", Get(""));
    EXPECT_EQ("", Get("empty-value"));
    EXPECT_EQ(binary_value, Get(binary_key));
    EXPECT_EQ("NOT_FOUND", Get(std::string("\x00\xff", 2)));
    // Same answers from the memtable, then from a table, then after reopen.
    if (round == 0) {
      ASSERT_OK(db_->Flush());
    } else if (round == 1) {
      ASSERT_OK(Reopen());
    }
  }
}

TEST_F(DBTest, LargeValues) {
  std::mt19937_64 rng(1);
  const std::string one_mib = test::RandomBytes(&rng, 1 << 20);
  const std::string larger_than_buffer = test::RandomBytes(&rng, 6 << 20);
  ASSERT_OK(Put("a", one_mib));
  ASSERT_OK(Put("b", larger_than_buffer));
  ASSERT_OK(Put("c", "small"));
  EXPECT_EQ(one_mib, Get("a"));
  EXPECT_EQ(larger_than_buffer, Get("b"));
  ASSERT_OK(db_->Flush());
  EXPECT_EQ(larger_than_buffer, Get("b"));
  ASSERT_OK(Reopen());
  EXPECT_EQ(one_mib, Get("a"));
  EXPECT_EQ(larger_than_buffer, Get("b"));
  EXPECT_EQ("small", Get("c"));
}

TEST_F(DBTest, WriteBatchAppliesItsOperationsInOrder) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Delete("a");
  batch.Put("a", "2");
  batch.Put("b", "1");
  batch.Delete("b");
  batch.Put("c", "x");
  ASSERT_OK(db_->Write(WriteOptions(), batch));
  EXPECT_EQ("2", Get("a"));
  EXPECT_EQ("NOT_FOUND", Get("b"));
  EXPECT_EQ("x", Get("c"));
  ASSERT_OK(Reopen());
  EXPECT_EQ("2", Get("a"));
  EXPECT_EQ("NOT_FOUND", Get("b"));

  ASSERT_OK(db_->Write(WriteOptions(), WriteBatch()));  // Empty batches are fine.
}

TEST_F(DBTest, ScanReturnsKeysInUnsignedByteOrder) {
  const std::vector<std::string> keys = {
      std::string("\xff", 1), std::string("\x80", 1), "ab",    std::string("a\0", 2), "a",
      std::string("\x7f", 1), std::string("\x01", 1), "",
  };
  for (const std::string& key : keys) {
    ASSERT_OK(Put(key, "v"));
  }
  const std::vector<std::string> expected = {
      "", std::string("\x01", 1), "a", std::string("a\0", 2), "ab", std::string("\x7f", 1),
      std::string("\x80", 1), std::string("\xff", 1),
  };
  std::vector<std::string> scanned;
  for (const auto& [key, value] : Scan()) {
    scanned.push_back(key);
  }
  EXPECT_EQ(expected, scanned);
}

TEST_F(DBTest, ScanMatchesAModelAcrossMemtableAndTables) {
  std::mt19937_64 rng(8);
  std::map<std::string, std::string> model;
  for (int i = 0; i < 3000; ++i) {
    const std::string key = test::RandomBytes(&rng, rng() % 6);
    if (rng() % 4 == 0) {
      ASSERT_OK(Delete(key));
      model.erase(key);
    } else {
      const std::string value = test::RandomBytes(&rng, rng() % 30);
      ASSERT_OK(Put(key, value));
      model[key] = value;
    }
    if (i % 700 == 699) {
      ASSERT_OK(db_->Flush());
    }
  }
  EXPECT_EQ(model, Scan());
  ASSERT_OK(Reopen());
  EXPECT_EQ(model, Scan());
}

TEST_F(DBTest, IteratorSeek) {
  for (uint64_t i = 0; i < 100; i += 2) {
    ASSERT_OK(Put(Key(i), "v" + std::to_string(i)));
    if (i == 50) {
      ASSERT_OK(db_->Flush());  // Half the keys in a table, half in memory.
    }
  }
  auto iter = db_->NewIterator(ReadOptions());
  EXPECT_FALSE(iter->Valid());  // Unpositioned until a seek.
  iter->Seek(Key(5));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(Key(6), iter->key());
  EXPECT_EQ("v6", iter->value());
  iter->Next();
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(Key(8), iter->key());
  iter->Seek(Key(52));
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(Key(52), iter->key());
  iter->Seek("");
  ASSERT_TRUE(iter->Valid());
  EXPECT_EQ(Key(0), iter->key());
  iter->Seek(Key(99));
  EXPECT_FALSE(iter->Valid());
  EXPECT_OK(iter->status());
}

TEST_F(DBTest, IteratorIsASnapshot) {
  ASSERT_OK(Put("a", "1"));
  ASSERT_OK(Put("b", "1"));
  ASSERT_OK(Put("c", "1"));
  auto iter = db_->NewIterator(ReadOptions());

  ASSERT_OK(Put("a", "2"));
  ASSERT_OK(Delete("b"));
  ASSERT_OK(Put("d", "1"));
  // The flush retires the memtable the iterator reads; the iterator keeps
  // it alive.
  ASSERT_OK(FlushAndSettle());

  std::map<std::string, std::string> seen;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    seen.emplace(iter->key(), iter->value());
  }
  ASSERT_OK(iter->status());
  EXPECT_EQ((std::map<std::string, std::string>{{"a", "1"}, {"b", "1"}, {"c", "1"}}), seen);
  iter.reset();

  EXPECT_EQ((std::map<std::string, std::string>{{"a", "2"}, {"c", "1"}, {"d", "1"}}), Scan());
}

TEST_F(DBTest, PersistsAcrossReopen) {
  std::map<std::string, std::string> model;
  for (int cycle = 0; cycle < 4; ++cycle) {
    for (uint64_t i = 0; i < 300; ++i) {
      const std::string key = Key((i * 7 + static_cast<uint64_t>(cycle)) % 400);
      if (i % 9 == 0) {
        ASSERT_OK(Delete(key));
        model.erase(key);
      } else {
        const std::string value = "c" + std::to_string(cycle) + "-" + std::to_string(i);
        ASSERT_OK(Put(key, value));
        model[key] = value;
      }
    }
    if (cycle % 2 == 1) {
      ASSERT_OK(db_->Flush());
    }
    ASSERT_OK(Reopen());
    ASSERT_EQ(model, Scan()) << "cycle " << cycle;
  }
  for (const auto& [key, value] : model) {
    EXPECT_EQ(value, Get(key));
  }
}

// Each flush makes a level-0 table; reads must take every key from the
// newest place that mentions it: the memtable, then tables newest first. A
// tombstone hides all older values, even ones in older tables. Compaction
// is off so that the tables stay in level 0.
TEST_F(DBTest, ShadowingAcrossMemtableAndSeveralLevel0Tables) {
  options_.disable_auto_compactions = true;
  ASSERT_OK(Reopen());
  std::map<std::string, std::string> model;
  auto put = [&](uint64_t i, const std::string& tag) {
    const std::string value = tag + "-" + std::to_string(i);
    ASSERT_OK(Put(Key(i), value));
    model[Key(i)] = value;
  };
  auto del = [&](uint64_t i) {
    ASSERT_OK(Delete(Key(i)));
    model.erase(Key(i));
  };

  for (uint64_t i = 0; i < 200; ++i) put(i, "t1");
  ASSERT_OK(db_->Flush());
  for (uint64_t i = 0; i < 200; i += 2) put(i, "t2");
  ASSERT_OK(db_->Flush());
  for (uint64_t i = 0; i < 200; i += 3) del(i);  // A table of tombstones.
  ASSERT_OK(db_->Flush());
  for (uint64_t i = 0; i < 200; i += 5) put(i, "t4");  // Revives some.
  ASSERT_OK(db_->Flush());
  ASSERT_EQ(4, FilesAtLevel(0));
  // The memtable layer.
  for (uint64_t i = 0; i < 210; i += 7) del(i);
  for (uint64_t i = 0; i < 210; i += 11) put(i, "mem");

  auto check = [&](const char* when) {
    for (uint64_t i = 0; i < 215; ++i) {
      const auto it = model.find(Key(i));
      EXPECT_EQ(it == model.end() ? "NOT_FOUND" : it->second, Get(Key(i))) << Key(i) << " " << when;
    }
    EXPECT_EQ(model, Scan()) << when;
  };
  check("with a memtable over four tables");

  // Recovery replays the memtable's log into a fifth table.
  ASSERT_OK(Reopen());
  EXPECT_EQ(5, FilesAtLevel(0));
  check("after reopen");
}

TEST_F(DBTest, ImmutableMemtableIsSearchedBetweenMemtableAndTables) {
  ASSERT_OK(Put("a", "table"));
  ASSERT_OK(Put("b", "table"));
  ASSERT_OK(Put("c", "table"));
  ASSERT_OK(Put("d", "table"));
  ASSERT_OK(db_->Flush());

  impl()->TEST_PauseBackgroundWork();
  ASSERT_OK(Put("a", "imm"));
  ASSERT_OK(Put("b", "imm"));
  ASSERT_OK(Delete("c"));
  ASSERT_OK(impl()->TEST_SwitchMemTable());
  ASSERT_TRUE(impl()->TEST_HasImmutableMemTable());
  ASSERT_OK(Put("a", "mem"));
  ASSERT_OK(Delete("d"));

  const std::map<std::string, std::string> expected = {{"a", "mem"}, {"b", "imm"}};
  EXPECT_EQ("mem", Get("a"));
  EXPECT_EQ("imm", Get("b"));
  EXPECT_EQ("NOT_FOUND", Get("c"));
  EXPECT_EQ("NOT_FOUND", Get("d"));
  EXPECT_EQ(expected, Scan());

  impl()->TEST_ResumeBackgroundWork();
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_FALSE(impl()->TEST_HasImmutableMemTable());
  EXPECT_EQ(2, FilesAtLevel(0));
  EXPECT_EQ(expected, Scan());
}

TEST_F(DBTest, FlushMovesDataToATableAndRetiresTheLog) {
  for (uint64_t i = 0; i < 100; ++i) {
    ASSERT_OK(Put(Key(i), "value"));
  }
  const std::vector<std::string> logs_before = DbFiles(".log");
  ASSERT_EQ(1u, logs_before.size());
  EXPECT_TRUE(DbFiles(".sst").empty());

  ASSERT_OK(FlushAndSettle());
  EXPECT_EQ(1, FilesAtLevel(0));
  EXPECT_EQ(1u, DbFiles(".sst").size());
  const std::vector<std::string> logs_after = DbFiles(".log");
  ASSERT_EQ(1u, logs_after.size());
  EXPECT_NE(logs_before, logs_after);  // The old log was deleted.
  EXPECT_TRUE(DbFiles(".tmp").empty());
  EXPECT_EQ(1u, test::FilesWithSuffix(dbname_, "CURRENT").size());
  int manifests = 0;
  for (const std::string& name : test::ListDir(dbname_)) {
    manifests += name.starts_with("MANIFEST-") ? 1 : 0;
  }
  EXPECT_EQ(1, manifests);
  for (uint64_t i = 0; i < 100; ++i) {
    EXPECT_EQ("value", Get(Key(i)));
  }
}

TEST_F(DBTest, FlushWithNothingBufferedIsANoop) {
  ASSERT_OK(FlushAndSettle());
  EXPECT_EQ(0, FilesAtLevel(0));
  ASSERT_OK(Put("k", "v"));
  ASSERT_OK(FlushAndSettle());
  ASSERT_OK(FlushAndSettle());
  EXPECT_EQ(1, FilesAtLevel(0));
}

TEST_F(DBTest, FullMemtableIsFlushedAutomatically) {
  UseNoSyncEnv();
  options_.write_buffer_size = 4096;
  options_.disable_auto_compactions = true;  // Count the flushes in level 0.
  ASSERT_OK(Reopen());
  const std::string value(100, 'x');
  for (uint64_t i = 0; i < 1000; ++i) {
    ASSERT_OK(Put(Key(i), value));
  }
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_GE(FilesAtLevel(0), 10);  // About 100 KB written through a 4 KB buffer.
  for (uint64_t i = 0; i < 1000; ++i) {
    ASSERT_EQ(value, Get(Key(i)));
  }
  ASSERT_OK(Reopen());
  EXPECT_EQ(1000u, Scan().size());
}

// At the smallest write buffer, small writes share a memtable instead of
// each one switching logs and flushing a one-entry table.
TEST_F(DBTest, SmallestWriteBufferHoldsManySmallWrites) {
  UseNoSyncEnv();
  options_.write_buffer_size = 4096;
  options_.statistics = std::make_shared<Statistics>();
  ASSERT_OK(Reopen());
  for (int i = 0; i < 20; ++i) {
    ASSERT_OK(Put(Key(static_cast<uint64_t>(i)), "v"));
  }
  impl()->TEST_WaitForBackgroundIdle();
  EXPECT_EQ(0u, options_.statistics->flushes.load());
  EXPECT_EQ(0, FilesAtLevel(0));
}

// Recovery must restart sequence numbers above every number already in a
// table or log; otherwise an older version could shadow a newer one.
TEST_F(DBTest, SequenceNumbersStayMonotonicAcrossRestarts) {
  ASSERT_OK(Put("k", "v1"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Reopen());
  ASSERT_OK(Put("k", "v2"));
  EXPECT_EQ("v2", Get("k"));
  ASSERT_OK(Reopen());  // v2 is replayed from the log into a table.
  EXPECT_EQ("v2", Get("k"));
  ASSERT_OK(Reopen());
  ASSERT_OK(Put("k", "v3"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Reopen());
  EXPECT_EQ("v3", Get("k"));
  ASSERT_OK(Delete("k"));
  ASSERT_OK(Reopen());
  EXPECT_EQ("NOT_FOUND", Get("k"));
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Reopen());
  EXPECT_EQ("NOT_FOUND", Get("k"));
}

TEST_F(DBTest, SyncWritesAreReadable) {
  for (uint64_t i = 0; i < 20; ++i) {
    ASSERT_OK(Put(Key(i), "synced", /*sync=*/true));
  }
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 20; ++i) {
    EXPECT_EQ("synced", Get(Key(i)));
  }
}

TEST_F(DBTest, Properties) {
  std::string value;
  ASSERT_TRUE(db_->GetProperty("kvlog.num-files-at-level0", &value));
  EXPECT_EQ("0", value);
  ASSERT_OK(Put("k", std::string(1000, 'v')));
  ASSERT_TRUE(db_->GetProperty("kvlog.approximate-memory-usage", &value));
  EXPECT_GT(std::stoull(value), 1000u);
  ASSERT_OK(db_->Flush());
  ASSERT_TRUE(db_->GetProperty("kvlog.num-files-at-level0", &value));
  EXPECT_EQ("1", value);
  ASSERT_TRUE(db_->GetProperty("kvlog.num-files-at-level6", &value));
  EXPECT_EQ("0", value);
  ASSERT_TRUE(db_->GetProperty("kvlog.sstables", &value));
  EXPECT_NE(std::string::npos, value.find("--- level 0 ---"));
  ASSERT_TRUE(db_->GetProperty("kvlog.stats", &value));
  EXPECT_NE(std::string::npos, value.find("flushes: 1")) << value;

  for (const char* bad : {"kvlog.num-files-at-level7", "kvlog.num-files-at-level-1",
                          "kvlog.num-files-at-level", "kvlog.num-files-at-level0x", "kvlog.nope",
                          "leveldb.stats", ""}) {
    EXPECT_FALSE(db_->GetProperty(bad, &value)) << bad;
  }
}

TEST_F(DBTest, OutOfRangeOptionsAreClamped) {
  UseNoSyncEnv();
  options_.write_buffer_size = 1;
  options_.block_size = 1;
  options_.block_restart_interval = 0;
  options_.max_open_files = 0;
  options_.max_manifest_file_size = 0;
  ASSERT_OK(Reopen());
  for (uint64_t i = 0; i < 300; ++i) {
    ASSERT_OK(Put(Key(i), "v"));
  }
  ASSERT_OK(db_->Flush());
  ASSERT_OK(Reopen());
  EXPECT_EQ(300u, Scan().size());
}

// ---------------------------------------------------------------------------
// Opening, locking and destroying

class DBOpenTest : public testing::Test {
 protected:
  test::TempDir dir_;
  const std::string dbname_ = dir_.Sub("db");
};

TEST_F(DBOpenTest, MissingDatabaseNeedsCreateIfMissing) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(Options(), dbname_, &db);
  EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
  EXPECT_EQ(nullptr, db);
  EXPECT_TRUE(test::ListDir(dir_.path()).empty());  // Nothing was created.

  Options options;
  options.create_if_missing = true;
  ASSERT_OK(DB::Open(options, dbname_, &db));
  ASSERT_NE(nullptr, db);
  db.reset();
  ASSERT_OK(DB::Open(Options(), dbname_, &db));  // Now it exists.
}

TEST_F(DBOpenTest, ErrorIfExists) {
  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, dbname_, &db));
  db.reset();
  options.error_if_exists = true;
  EXPECT_TRUE(DB::Open(options, dbname_, &db).IsInvalidArgument());
}

TEST_F(DBOpenTest, LockPreventsASecondOpen) {
  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> first;
  ASSERT_OK(DB::Open(options, dbname_, &first));
  std::unique_ptr<DB> second;
  const Status s = DB::Open(options, dbname_, &second);
  EXPECT_TRUE(s.IsBusy()) << s.ToString();
  EXPECT_EQ(nullptr, second);
  EXPECT_TRUE(DestroyDB(dbname_, options).IsBusy());

  ASSERT_OK(first->Put(WriteOptions(), "k", "v"));
  first.reset();
  ASSERT_OK(DB::Open(options, dbname_, &second));
  EXPECT_EQ("v", test::GetOrStatus(second.get(), "k"));
}

TEST_F(DBOpenTest, DestroyDBRemovesTheDatabase) {
  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, dbname_, &db));
  ASSERT_OK(db->Put(WriteOptions(), "k", "v"));
  ASSERT_OK(db->Flush());
  db.reset();
  ASSERT_OK(DestroyDB(dbname_, options));
  EXPECT_TRUE(test::ListDir(dir_.path()).empty());
  ASSERT_OK(DestroyDB(dbname_, options));  // Nothing left to destroy.
}

TEST_F(DBOpenTest, ForeignFilesAreLeftAlone) {
  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, dbname_, &db));
  test::WriteFileBytes(dbname_ + "/notes.txt", "mine");
  test::WriteFileBytes(dbname_ + "/000123.backup", "mine too");
  ASSERT_OK(db->Put(WriteOptions(), "k", "v"));
  ASSERT_OK(db->Flush());
  db.reset();
  ASSERT_OK(DB::Open(options, dbname_, &db));
  db.reset();
  ASSERT_OK(DestroyDB(dbname_, options));
  EXPECT_EQ((std::vector<std::string>{"000123.backup", "notes.txt"}), test::ListDir(dbname_));
}

TEST_F(DBOpenTest, MissingCurrentWithDataIsCorruption) {
  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_OK(DB::Open(options, dbname_, &db));
  ASSERT_OK(db->Put(WriteOptions(), "k", "v"));
  db.reset();
  test::RemoveFile(dbname_ + "/CURRENT");
  // Starting a new, empty database here would silently drop the data.
  const Status s = DB::Open(options, dbname_, &db);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

}  // namespace
}  // namespace kvlog
