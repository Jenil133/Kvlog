#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "kvlog/iterator.h"
#include "kvlog/options.h"
#include "kvlog/status.h"
#include "kvlog/write_batch.h"

namespace kvlog {

// A persistent, ordered key-value store. Keys and values are arbitrary byte
// strings (empty ones included) ordered by unsigned bytewise comparison.
//
// A DB is safe for concurrent use by multiple threads. Destroying it closes
// the database; every iterator obtained from it must be destroyed first.
class DB {
 public:
  // Opens the database stored in directory `path`. On success stores the
  // handle in *db; on failure leaves *db empty and returns the error. Fails
  // with Busy if another DB instance (in any process) has the database open.
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  virtual ~DB();

  // Sets the value for `key`, replacing any previous value.
  virtual Status Put(const WriteOptions& options, std::string_view key,
                     std::string_view value) = 0;

  // Removes `key`. Deleting a missing key is not an error.
  virtual Status Delete(const WriteOptions& options, std::string_view key) = 0;

  // Applies every operation in `batch` atomically.
  virtual Status Write(const WriteOptions& options, const WriteBatch& batch) = 0;

  // Stores the value for `key` in *value. Returns NotFound if the key is
  // absent. Other errors (Corruption, IOError) mean the read itself failed.
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;

  // Returns an iterator over the user keys and values of a consistent
  // snapshot taken now; later writes are not visible through it. The
  // iterator starts unpositioned: call SeekToFirst() or Seek() first.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  // Writes everything currently buffered in memory to a table file and waits
  // until that file is part of the database. Afterwards every write
  // acknowledged before the call lives in a table file, not just the log.
  virtual Status Flush() = 0;

  // Compacts every key in [*begin, *end] (a null bound is unbounded) down
  // the levels until it shares a level with all older data of the same
  // keys: overwritten versions and deleted keys in the range are removed
  // from disk. Flushes the memtable first. Runs on the background thread;
  // the caller waits until it is done. Works even when
  // disable_auto_compactions is set.
  virtual Status CompactRange(const std::string* begin, const std::string* end) = 0;

  // Blocks until no flush or compaction is pending or running. Returns
  // early if background work has failed (writes then report the error).
  virtual void WaitForBackgroundWork() = 0;

  // Reports an internal statistic. Returns false for unknown properties.
  //   "kvlog.num-files-at-level<N>"   number of table files at level N
  //   "kvlog.sstables"                 table files per level, one per line
  //   "kvlog.approximate-memory-usage" bytes held by in-memory tables
  //   "kvlog.stats"                    human-readable summary: files, bytes and
  //                                    compactions per level, followed by the
  //                                    Options::statistics counters if set
  virtual bool GetProperty(std::string_view property, std::string* value) = 0;
};

// Deletes every file of the database stored in `path`, then the directory if
// it is empty. Fails with Busy if the database is open.
Status DestroyDB(const std::string& path, const Options& options);

}  // namespace kvlog
