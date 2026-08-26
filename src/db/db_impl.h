#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "kvlog/db.h"
#include "kvlog/env.h"
#include "kvlog/write_batch.h"

namespace kvlog {

// The database implementation.
//
// Threading model:
//  - mutex_ guards all mutable state below unless a comment says otherwise.
//  - Writers queue up in writers_. The writer at the front (the leader)
//    merges the queued batches into one group, then appends the group to the
//    log and inserts it into the memtable *without* holding mutex_; only the
//    leader may touch log_ or insert into mem_, so that is safe. Concurrent
//    readers are fine: the memtable supports one writer plus lock-free
//    readers, and a group only becomes visible when the leader publishes its
//    last sequence number under mutex_.
//  - Reads copy the shared_ptrs to mem_, imm_ and the current version under
//    mutex_, then search them without it.
//  - One background thread flushes the immutable memtable (imm_) to a
//    level-0 table and commits it to the MANIFEST, and runs compactions
//    (see compaction.h), releasing mutex_ for the file I/O. Flushes come
//    first: a running compaction checks for an immutable memtable between
//    entries and flushes it right away, so writers never wait for a whole
//    compaction. Being the only thread that installs versions keeps
//    VersionSet::LogAndApply single-caller.
//  - Writes are slowed down, then stopped, while level 0 holds too many
//    files (level0_slowdown_writes_trigger, level0_stop_writes_trigger), so
//    compaction can keep up and reads stay bounded.
class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, const std::string& dbname);
  DBImpl(const DBImpl&) = delete;
  DBImpl& operator=(const DBImpl&) = delete;
  ~DBImpl() override;

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  Status Flush() override;
  Status CompactRange(const std::string* begin, const std::string* end) override;
  void WaitForBackgroundWork() override;
  bool GetProperty(std::string_view property, std::string* value) override;

  // Hooks for tests.

  // Stops the background thread from starting new work, waiting for any
  // flush in progress to finish. Writers that need room while it is paused
  // block, so tests keep the memtable from filling.
  void TEST_PauseBackgroundWork();
  void TEST_ResumeBackgroundWork();
  // Waits until the background thread has nothing left to do: no flush or
  // compaction is pending or running, including the obsolete-file cleanup
  // that follows each. Also returns if background work is paused or has
  // failed. Same as WaitForBackgroundWork().
  void TEST_WaitForBackgroundIdle();
  // Compacts the files of `level` overlapping [*begin, *end] (null bounds
  // are unbounded) into level + 1, without flushing first.
  Status TEST_CompactRange(int level, const std::string* begin, const std::string* end);
  // While paused, automatic compactions are not started (flushes and manual
  // compactions still run), so level 0 can be grown on purpose.
  void TEST_SetAutoCompactionsPaused(bool paused);
  // Waits until some writer is blocked by level0_stop_writes_trigger.
  void TEST_WaitForStoppedWriter();
  std::shared_ptr<Version> TEST_CurrentVersion();
  // The options in effect, after DB::Open clamped them.
  const Options& TEST_Options() const { return options_; }
  // Freezes the memtable (if it has entries) without waiting for the flush.
  // Requires: no immutable memtable exists.
  Status TEST_SwitchMemTable();
  bool TEST_HasImmutableMemTable();
  uint64_t TEST_LogFileNumber();
  Status TEST_BackgroundError();

 private:
  friend class DB;
  struct Writer;
  struct CompactionState;

  // Per output level: compactions that wrote into it and their I/O. Trivial
  // moves (a file relinked one level down) are counted separately: they
  // read and write nothing.
  struct LevelStats {
    uint64_t compactions = 0;
    uint64_t trivial_moves = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_written = 0;
  };

  struct Stats {
    // Memtables written to level-0 tables, by the background thread or by
    // recovery, and the bytes of the tables written.
    uint64_t flushes = 0;
    uint64_t flushed_bytes = 0;
    uint64_t write_stalls = 0;  // Writers that waited for a flush to finish.
    uint64_t write_slowdowns = 0;  // Writes delayed by the level-0 slowdown trigger.
    uint64_t write_stops = 0;  // Times a writer blocked on the level-0 stop trigger.
    uint64_t recovered_batches = 0;  // Write batches replayed when this database opened.
    LevelStats levels[kNumLevels];
  };

  // A CompactRange step waiting for (or being run by) the background thread.
  // Lives on the requesting thread's stack until `done`.
  struct ManualCompaction {
    int level = 0;
    const InternalKey* begin = nullptr;  // nullptr: unbounded.
    const InternalKey* end = nullptr;
    bool in_progress = false;  // Picked up by the background thread.
    bool done = false;
    Status status;
  };

  // Recovers the on-disk state, starts a fresh log and the background
  // thread. Called once, by DB::Open.
  Status OpenImpl();
  Status Recover(VersionEdit* edit, std::unique_lock<std::mutex>& lock);
  Status NewDB();
  // Replays one log into level-0 tables. `later_logs_have_data` says whether
  // any log after this one holds data: a torn tail is then damage, since
  // every log is synced before the next one takes writes.
  Status RecoverLogFile(uint64_t log_number, bool later_logs_have_data, VersionEdit* edit,
                        SequenceNumber* max_sequence, bool* stop,
                        std::unique_lock<std::mutex>& lock);
  // Copies the logs that point-in-time recovery stopped in or skipped into
  // the lost/ subdirectory with a note, durably, before the MANIFEST edit
  // that makes the originals obsolete is committed: they hold acknowledged
  // writes that may still be salvaged by hand, so they must never be deleted
  // without a copy surviving. A failure here fails the open.
  Status QuarantineSkippedLogs();

  // Writes `mem` to a new level-0 table and records it in `edit`. The file
  // number is added to pending_outputs_ and returned in *file_number; the
  // caller removes it once the edit is committed (or abandoned).
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, std::unique_lock<std::mutex>& lock,
                          uint64_t* file_number);

  // Commits `batch` through the writer queue. A null batch is a flush
  // request: it freezes the memtable if it holds anything and stores the
  // memtable whose flush the caller should wait for in *flush_target.
  Status WriteImpl(const WriteOptions& options, const WriteBatch* batch,
                   std::shared_ptr<MemTable>* flush_target);
  // Requires: the caller is the leader. Ensures the memtable has room
  // (or, if `force`, freezes it), waiting for the background flush if both
  // memtables are full.
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force);
  // Freezes mem_ into imm_ and starts a new memtable and log.
  Status SwitchMemTable(std::unique_lock<std::mutex>& lock);
  // Merges the batches of the leader and the writers queued behind it.
  const WriteBatch* BuildBatchGroup(Writer** last_writer);

  void RecordBackgroundError(const Status& s);
  // Adds `n` to a counter of options_.statistics, if one is attached.
  void RecordStatistic(std::atomic<uint64_t> Statistics::*counter, uint64_t n = 1) const;

  // True if the background thread has a flush or compaction to run.
  bool BackgroundWorkPending() const;
  // Waits until BackgroundWorkPending() is false and nothing is running, or
  // background work is paused, has failed or is shutting down.
  void WaitForBackgroundIdle(std::unique_lock<std::mutex>& lock);
  void BackgroundMain();
  // Runs one unit of background work: a flush if one is pending, else a
  // compaction.
  void BackgroundWork(std::unique_lock<std::mutex>& lock);
  void FlushImmutableMemTable(std::unique_lock<std::mutex>& lock);
  void RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock);

  // Merges the compaction's inputs into new files of the output level and
  // installs the result. Releases mutex_ while merging.
  Status DoCompactionWork(CompactionState* compact, std::unique_lock<std::mutex>& lock);
  // Requires: mutex_ not held.
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact);
  Status InstallCompactionResults(CompactionState* compact, std::unique_lock<std::mutex>& lock);
  // Hands one CompactRange step to the background thread and waits for it.
  Status RunManualCompaction(int level, const std::string* begin, const std::string* end);

  // Immutable after construction.
  const Options options_;
  Env* const env_;
  const std::string dbname_;
  const InternalKeyComparator internal_comparator_;
  const std::unique_ptr<TableCache> table_cache_;  // Thread-safe on its own.
  std::unique_ptr<FileLock> db_lock_;

  std::mutex mutex_;
  // Wakes the background thread: new work, resume, or shutdown.
  std::condition_variable bg_work_cv_;
  // Signals that background work finished (a flush or compaction completed
  // or failed), and that a writer stopped on the level-0 trigger.
  std::condition_variable bg_done_cv_;
  // Written under mutex_; also read without it by a running compaction,
  // which gives up as soon as the database starts closing.
  std::atomic<bool> shutting_down_{false};
  bool bg_paused_ = false;
  bool bg_running_ = false;
  bool auto_compactions_paused_ = false;  // Test hook.
  ManualCompaction* manual_compaction_ = nullptr;
  int stopped_writers_ = 0;  // Writers blocked by level0_stop_writes_trigger.

  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // Being flushed; nullptr if none.
  // imm_ != nullptr, readable without mutex_: a compaction polls it between
  // entries to flush a waiting memtable promptly.
  std::atomic<bool> has_imm_{false};
  std::unique_ptr<log::Writer> log_;
  uint64_t logfile_number_ = 0;

  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;  // Holds merged groups; used only by the leader.

  // Table files being written that are not yet in the MANIFEST; never
  // deleted as garbage.
  std::set<uint64_t> pending_outputs_;

  std::unique_ptr<VersionSet> versions_;

  // Set when a write to the log or a background flush fails. Sticky: every
  // later write fails with it until the database is reopened.
  Status bg_error_;

  Stats stats_;
  // Describes damage that recovery tolerated (paranoid_checks == false).
  std::string recovery_note_;
  // Logs point-in-time recovery did not fully replay; see
  // QuarantineSkippedLogs().
  std::vector<uint64_t> skipped_logs_;

  // Iterators that are still alive; checked when the database closes.
  std::atomic<int> live_iterators_{0};

  std::thread bg_thread_;
};

}  // namespace kvlog
