#include "db/db_impl.h"

#include <sys/resource.h>

#include <algorithm>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "db/builder.h"
#include "db/compaction.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/log_format.h"
#include "db/log_reader.h"
#include "db/write_batch_internal.h"
#include "table/merging_iterator.h"
#include "table/table_builder.h"
#include "util/cache.h"
#include "util/coding.h"
#include "util/env_util.h"

namespace kvlog {

// A writer waiting in the queue. Lives on the waiting thread's stack.
struct DBImpl::Writer {
  const WriteBatch* batch = nullptr;  // nullptr: a flush request.
  bool sync = false;
  bool done = false;  // Set by a leader that committed this write.
  Status status;
  std::condition_variable cv;
};

// The outputs of one running compaction. Used only by the background thread.
struct DBImpl::CompactionState {
  explicit CompactionState(Compaction* c) : compaction(c) {}

  Compaction* const compaction;
  // Every output file started, in key order; the last one is being built
  // while `builder` is set.
  std::vector<FileMetaData> outputs;
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  uint64_t total_bytes = 0;  // Bytes of the finished outputs.
  // Set when the compaction gave up because the database is closing. Its
  // outputs were removed and the MANIFEST was never touched.
  bool abandoned = false;
};

namespace {

template <typename T>
void ClipToRange(T* value, T min, T max) {
  *value = std::clamp(*value, min, max);
}

Options SanitizeOptions(const Options& src) {
  Options result = src;
  if (result.env == nullptr) {
    result.env = Env::Default();
  }
  ClipToRange<size_t>(&result.write_buffer_size, size_t{4} << 10, size_t{1} << 30);
  ClipToRange<size_t>(&result.block_size, 256, size_t{4} << 20);
  ClipToRange(&result.block_restart_interval, 1, 1024);
  ClipToRange(&result.max_open_files, 16, 50000);
  // Each cached table holds a file descriptor. A cache allowed to exceed the
  // process limit would make reads fail with EMFILE and, worse, make creating
  // the next log, table or MANIFEST fail, which stops all writes. Leave room
  // for those and for the application's own descriptors.
  if (struct rlimit limit; ::getrlimit(RLIMIT_NOFILE, &limit) == 0 &&
                           limit.rlim_cur != RLIM_INFINITY) {
    constexpr rlim_t kReservedDescriptors = 64;
    const rlim_t usable = limit.rlim_cur > kReservedDescriptors + 16
                              ? limit.rlim_cur - kReservedDescriptors
                              : 16;
    if (static_cast<rlim_t>(result.max_open_files) > usable) {
      result.max_open_files = static_cast<int>(usable);
    }
  }
  ClipToRange<uint64_t>(&result.max_manifest_file_size, 1, uint64_t{1} << 30);

  // Compaction. The write triggers are kept ordered (compaction <= slowdown
  // <= stop): stopping writes below the compaction trigger would block
  // writers with no compaction scheduled to unblock them.
  ClipToRange(&result.level0_compaction_trigger, 1, 1000);
  ClipToRange(&result.level0_slowdown_writes_trigger, result.level0_compaction_trigger, 1000);
  ClipToRange(&result.level0_stop_writes_trigger, result.level0_slowdown_writes_trigger, 1000);
  ClipToRange<uint64_t>(&result.max_bytes_for_level_base, uint64_t{1} << 10, uint64_t{1} << 40);
  ClipToRange(&result.max_bytes_for_level_multiplier, 2, 100);
  ClipToRange<uint64_t>(&result.target_file_size, uint64_t{1} << 10, uint64_t{1} << 30);
  ClipToRange<uint32_t>(&result.compaction_throttle_us, 0, 1'000'000);

  // Reads.
  ClipToRange(&result.bloom_bits_per_key, 0, 64);
  ClipToRange<size_t>(&result.block_cache_capacity, 0, size_t{1} << 40);
  return result;
}

}  // namespace

DBImpl::DBImpl(const Options& options, const std::string& dbname)
    : options_(SanitizeOptions(options)),
      env_(options_.env),
      dbname_(dbname),
      internal_comparator_(BytewiseComparator()),
      table_cache_(std::make_unique<TableCache>(dbname_, options_, &internal_comparator_,
                                                static_cast<size_t>(options_.max_open_files))),
      versions_(std::make_unique<VersionSet>(dbname_, &options_, table_cache_.get(),
                                             &internal_comparator_)) {}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    shutting_down_.store(true, std::memory_order_release);
  }
  bg_work_cv_.notify_all();
  if (bg_thread_.joinable()) {
    // Lets a flush in progress finish; a running compaction gives up and
    // removes its outputs. An unflushed memtable is not lost: its log is
    // still live and is replayed by the next Open.
    bg_thread_.join();
  }
  assert(live_iterators_.load() == 0 && "iterators must be destroyed before their DB");

  if (log_ != nullptr) {
    // A clean close makes every acknowledged write durable, including the
    // ones written with sync == false.
    if (bg_error_.ok()) {
      static_cast<void>(log_->Sync());
    }
    static_cast<void>(log_->Close());
    log_.reset();
  }
  versions_.reset();
  if (db_lock_ != nullptr) {
    static_cast<void>(env_->UnlockFile(std::move(db_lock_)));
  }
}

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  db->reset();
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->OpenImpl();
  if (s.ok()) {
    *db = std::move(impl);
  }
  return s;
}

Status DBImpl::OpenImpl() {
  std::unique_lock<std::mutex> lock(mutex_);
  VersionEdit edit;
  Status s = Recover(&edit, lock);
  if (s.ok()) {
    s = QuarantineSkippedLogs();
  }
  bool commit_attempted = false;
  uint64_t log_number = 0;
  if (s.ok()) {
    // New writes go to a fresh log. Everything replayed from older logs is
    // now in the level-0 tables recorded in `edit`, so once the edit is
    // committed those logs are garbage.
    log_number = versions_->NewFileNumber();
    std::unique_ptr<WritableFile> file;
    s = env_->NewWritableFile(LogFileName(dbname_, log_number), &file);
    if (s.ok()) {
      s = env_->SyncDir(dbname_);
    }
    if (s.ok()) {
      edit.SetLogNumber(log_number);
      logfile_number_ = log_number;
      log_ = std::make_unique<log::Writer>(std::move(file));
      mem_ = std::make_shared<MemTable>(internal_comparator_);
      commit_attempted = true;
      s = versions_->LogAndApply(&edit, &lock);
    }
  }
  if (!s.ok() && !commit_attempted) {
    // Nothing refers to the tables replay wrote or to the new log, so a
    // failed open removes them and leaves the directory as it found it.
    // Once the MANIFEST update has been attempted it may be on disk, and
    // the files must stay.
    for (const uint64_t number : pending_outputs_) {
      table_cache_->Evict(number);
      static_cast<void>(env_->RemoveFile(TableFileName(dbname_, number)));
    }
    if (log_number != 0) {
      static_cast<void>(env_->RemoveFile(LogFileName(dbname_, log_number)));
    }
  }
  pending_outputs_.clear();
  if (s.ok()) {
    RemoveObsoleteFiles(lock);
    bg_thread_ = std::thread(&DBImpl::BackgroundMain, this);
  }
  return s;
}

Status DBImpl::QuarantineSkippedLogs() {
  if (skipped_logs_.empty()) {
    return Status::OK();
  }
  // Copies, not renames: until the MANIFEST edit below commits, the logs are
  // still live, and a failed or interrupted open must find them where they
  // were. RemoveObsoleteFiles deletes the originals only after the commit,
  // by which time the copies, the note and their directory entries are
  // durable. A repeated open overwrites the same copies.
  const std::string lost_dir = dbname_ + "/" + kLostDirName;
  static_cast<void>(env_->CreateDir(lost_dir));  // Fails harmlessly if it exists.
  std::string note = recovery_note_ + "\nlogs kept here:";
  Status s;
  for (const uint64_t number : skipped_logs_) {
    const std::string name = LogFileName(dbname_, number);
    const std::string base = name.substr(dbname_.size() + 1);
    s = CopyFileSynced(env_, name, lost_dir + "/" + base);
    if (!s.ok()) {
      return s;
    }
    note += " " + base;
  }
  note += "\n";
  const std::string note_name =
      lost_dir + "/RECOVERY-" + std::to_string(versions_->LastSequence()) + ".txt";
  s = WriteStringToFile(env_, note, note_name, /*sync=*/true);
  if (s.ok()) {
    s = env_->SyncDir(lost_dir);
  }
  if (s.ok()) {
    s = env_->SyncDir(dbname_);  // The entry for lost/ itself.
  }
  if (s.ok()) {
    recovery_note_ += std::string(" (logs kept in ") + kLostDirName + "/)";
    skipped_logs_.clear();
  }
  return s;
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(internal_comparator_.user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(std::move(file));
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = log.Sync();
    }
    const Status close_status = log.Close();
    if (s.ok()) {
      s = close_status;
    }
  }
  if (!s.ok()) {
    static_cast<void>(env_->RemoveFile(manifest));
    return s;
  }
  return SetCurrentFile(env_, dbname_, 1);
}

Status DBImpl::Recover(VersionEdit* edit, std::unique_lock<std::mutex>& lock) {
  Status create_status;
  if (options_.create_if_missing) {
    // Fails harmlessly when the directory exists; real problems show up when
    // taking the lock.
    create_status = env_->CreateDir(dbname_);
    if (create_status.ok()) {
      // The new directory's entry lives in its parent. Without syncing the
      // parent, power loss could take the whole database with it, including
      // writes acknowledged as durable.
      const size_t slash = dbname_.find_last_of('/');
      const std::string parent = slash == std::string::npos ? std::string(".")
                                 : slash == 0               ? std::string("/")
                                                            : dbname_.substr(0, slash);
      Status s = env_->SyncDir(parent);
      if (!s.ok()) {
        return s;
      }
    }
  }
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      if (options_.create_if_missing) {
        // The directory could not be created (a missing parent, say); that
        // error explains the failure better than the lock's.
        return create_status.ok() ? s : create_status;
      }
      return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
    }
    return s;
  }

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
    }
    // A directory with logs or tables but no CURRENT is a damaged database,
    // not an empty one; starting over would silently discard it.
    std::vector<std::string> filenames;
    s = env_->GetChildren(dbname_, &filenames);
    if (!s.ok()) {
      return s;
    }
    for (const std::string& filename : filenames) {
      uint64_t number = 0;
      FileType type;
      if (ParseFileName(filename, &number, &type) &&
          (type == FileType::kLogFile || type == FileType::kTableFile)) {
        return Status::Corruption(dbname_, "CURRENT is missing but log or table files exist");
      }
    }
    s = NewDB();
    if (!s.ok()) {
      return s;
    }
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
  }

  s = versions_->Recover();
  if (!s.ok()) {
    return s;
  }

  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }
  std::set<uint64_t> expected_tables;
  versions_->AddLiveFiles(&expected_tables);
  std::vector<uint64_t> logs;
  for (const std::string& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (!ParseFileName(filename, &number, &type)) {
      continue;
    }
    // A crash can leave files numbered beyond the MANIFEST's next-file
    // counter (a log started after the last MANIFEST update, an unfinished
    // table). Their numbers must never be handed out again.
    versions_->MarkFileNumberUsed(number);
    if (type == FileType::kTableFile) {
      expected_tables.erase(number);
    } else if (type == FileType::kLogFile && number >= versions_->LogNumber()) {
      logs.push_back(number);
    }
  }
  if (!expected_tables.empty()) {
    return Status::Corruption(
        std::to_string(expected_tables.size()) +
            " table file(s) listed in the MANIFEST are missing",
        TableFileName(dbname_, *expected_tables.begin()));
  }

  // Replay the logs that may hold writes not yet in tables, oldest first.
  std::sort(logs.begin(), logs.end());
  // later_data[i]: some log after logs[i] is non-empty. (A crash during an
  // Open leaves an empty new log after torn older ones; that is harmless.)
  std::vector<bool> later_data(logs.size(), false);
  for (size_t i = logs.size(); i-- > 1;) {
    uint64_t size = 0;
    s = env_->GetFileSize(LogFileName(dbname_, logs[i]), &size);
    if (!s.ok()) {
      return s;
    }
    later_data[i - 1] = later_data[i] || size > 0;
  }
  SequenceNumber max_sequence = versions_->LastSequence();
  for (size_t i = 0; i < logs.size(); ++i) {
    bool stop = false;
    s = RecoverLogFile(logs[i], later_data[i], edit, &max_sequence, &stop, lock);
    if (!s.ok()) {
      return s;
    }
    if (stop) {
      // Later logs hold writes made after the damaged region; applying them
      // would leave a gap in the history. They are kept aside, not deleted.
      skipped_logs_.assign(logs.begin() + static_cast<std::ptrdiff_t>(i), logs.end());
      break;
    }
  }
  versions_->SetLastSequence(max_sequence);
  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool later_logs_have_data,
                              VersionEdit* edit, SequenceNumber* max_sequence, bool* stop,
                              std::unique_lock<std::mutex>& lock) {
  const std::string fname = LogFileName(dbname_, log_number);
  std::unique_ptr<SequentialFile> file;
  Status s = env_->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }

  log::Reader reader(std::move(file));
  std::shared_ptr<MemTable> mem;
  std::string record;
  WriteBatch batch;
  Status damage;
  uint64_t applied_end = 0;  // Log offset just past the last batch applied.
  while (reader.ReadRecord(&record)) {
    if (record.size() < WriteBatchInternal::kHeaderSize) {
      damage = Status::Corruption("log record too small for a write batch");
      break;
    }
    WriteBatchInternal::SetContents(&batch, record);
    // Validate before applying anything, so a malformed batch is never
    // half-applied.
    damage = WriteBatchInternal::Validate(batch);
    if (!damage.ok()) {
      break;
    }
    const SequenceNumber first = WriteBatchInternal::Sequence(batch);
    const uint32_t count = WriteBatchInternal::Count(batch);
    if (mem == nullptr) {
      mem = std::make_shared<MemTable>(internal_comparator_);
    }
    s = WriteBatchInternal::InsertInto(batch, first, mem.get());
    if (!s.ok()) {
      return s;
    }
    if (count > 0) {
      *max_sequence = std::max(*max_sequence, first + count - 1);
    }
    applied_end = reader.valid_end();
    ++stats_.recovered_batches;
    if (mem->BytesAdded() > options_.write_buffer_size) {
      uint64_t table_number = 0;
      s = WriteLevel0Table(mem.get(), edit, lock, &table_number);
      mem.reset();
      if (!s.ok()) {
        return s;
      }
    }
  }

  // A malformed batch ends the loop with the reader still in kReading and
  // `damage` already set; otherwise the reader's final state says why the
  // log ended.
  if (damage.ok()) {
    switch (reader.state()) {
      case log::Reader::State::kEof:
        break;
      case log::Reader::State::kTornTail:
        // A torn final record is an append interrupted by the crash; it was
        // never acknowledged as durable. But a log is synced before a newer
        // one takes writes, so a torn tail followed by more data means a
        // sync did not reach the disk: later writes exist without this one.
        if (later_logs_have_data) {
          damage = Status::Corruption("torn record at the end of a log followed by newer logs");
        }
        break;
      case log::Reader::State::kCorruption:
        damage = reader.status();
        break;
      case log::Reader::State::kIOError:
      case log::Reader::State::kReading:
        return Status::IOError(fname, reader.status().message());
    }
  }
  if (!damage.ok()) {
    if (options_.paranoid_checks) {
      return Status::Corruption(fname, damage.message());
    }
    // Point-in-time recovery: keep every write before the damage and none
    // after it, in this log or any later one.
    *stop = true;
    recovery_note_ = "replay stopped at damaged log " + fname + " after offset " +
                     std::to_string(applied_end) + ": " + damage.message();
  }

  if (mem != nullptr) {
    uint64_t table_number = 0;
    s = WriteLevel0Table(mem.get(), edit, lock, &table_number);
  }
  return s;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                std::unique_lock<std::mutex>& lock, uint64_t* file_number) {
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  *file_number = meta.number;
  // Shield the file from RemoveObsoleteFiles until the MANIFEST lists it.
  pending_outputs_.insert(meta.number);

  Status s;
  {
    lock.unlock();
    std::unique_ptr<Iterator> iter = mem->NewIterator();
    s = BuildTable(dbname_, env_, options_, &internal_comparator_, table_cache_.get(), iter.get(),
                   &meta);
    lock.lock();
  }
  if (s.ok() && meta.file_size > 0) {
    edit->AddFile(0, meta);
    ++stats_.flushes;
    stats_.flushed_bytes += meta.file_size;
    RecordStatistic(&Statistics::flushes);
  }
  return s;
}

Status DBImpl::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, batch);
}

Status DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, batch);
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  // A batch becomes one log record, whose length field is 32 bits.
  if (WriteBatchInternal::ByteSize(batch) > log::kMaxPayloadSize) {
    return Status::InvalidArgument("write batch too large",
                                   std::to_string(WriteBatchInternal::ByteSize(batch)));
  }
  if (batch.Count() == 0) {
    return Status::OK();
  }
  return WriteImpl(options, &batch, nullptr);
}

Status DBImpl::Flush() {
  std::shared_ptr<MemTable> target;
  Status s = WriteImpl(WriteOptions(), nullptr, &target);
  if (!s.ok() || target == nullptr) {
    return s;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  bg_done_cv_.wait(lock, [&] { return imm_ != target || !bg_error_.ok(); });
  return imm_ == target ? bg_error_ : Status::OK();
}

Status DBImpl::WriteImpl(const WriteOptions& options, const WriteBatch* batch,
                         std::shared_ptr<MemTable>* flush_target) {
  Writer w;
  w.batch = batch;
  w.sync = options.sync;

  std::unique_lock<std::mutex> lock(mutex_);
  writers_.push_back(&w);
  w.cv.wait(lock, [&] { return w.done || &w == writers_.front(); });
  if (w.done) {
    return w.status;  // A leader committed this write as part of its group.
  }

  // This thread is the leader until it hands the queue on below.
  Status status = MakeRoomForWrite(lock, /*force=*/batch == nullptr);
  Writer* last_writer = &w;
  if (status.ok() && batch != nullptr) {
    const WriteBatch* group = BuildBatchGroup(&last_writer);
    const SequenceNumber first_sequence = versions_->LastSequence() + 1;
    const uint32_t count = WriteBatchInternal::Count(*group);
    assert(first_sequence + count - 1 <= kMaxSequenceNumber);
    MemTable* const mem = mem_.get();
    log::Writer* const log = log_.get();

    // Only the leader appends to the log or inserts into the memtable, so
    // the slow part runs without the mutex: readers proceed, and new writers
    // queue up to form the next group.
    lock.unlock();
    // The log record is the serialized batch with its sequence field set;
    // writing the sequence as a separate piece avoids copying the batch.
    char sequence_bytes[8];
    EncodeFixed64(sequence_bytes, first_sequence);
    const std::string_view contents = WriteBatchInternal::Contents(*group);
    status = log->AddRecord({std::string_view(sequence_bytes, sizeof(sequence_bytes)),
                             contents.substr(sizeof(sequence_bytes))});
    if (status.ok() && w.sync) {
      status = log->Sync();
    }
    if (status.ok()) {
      status = WriteBatchInternal::InsertInto(*group, first_sequence, mem);
    }
    lock.lock();

    if (status.ok()) {
      // Publishing the last sequence makes the whole group visible to new
      // reads at once.
      versions_->SetLastSequence(first_sequence + count - 1);
    } else {
      // The log may now end in a partial record, and anything appended after
      // it would be unreadable during recovery. Refuse all further writes.
      RecordBackgroundError(status);
    }
    if (group == &tmp_batch_) {
      tmp_batch_.Clear();
    }
  } else if (status.ok() && flush_target != nullptr) {
    *flush_target = imm_;
  }

  // Complete every write of the group, then wake the next leader.
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) {
      break;
    }
  }
  if (!writers_.empty()) {
    writers_.front()->cv.notify_one();
  }
  return status;
}

const WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* const first = writers_.front();
  assert(first->batch != nullptr);
  const WriteBatch* result = first->batch;
  size_t size = WriteBatchInternal::ByteSize(*first->batch);

  // Cap the group so that one huge group does not delay its small writers
  // too much; small leaders get a smaller cap.
  size_t max_size = size_t{1} << 20;
  if (size <= (size_t{128} << 10)) {
    max_size = size + (size_t{128} << 10);
  }

  *last_writer = first;
  for (auto it = writers_.begin() + 1; it != writers_.end(); ++it) {
    Writer* const w = *it;
    if (w->batch == nullptr) {
      break;  // A flush request ends the group.
    }
    if (w->sync && !first->sync) {
      break;  // A sync write must not be acknowledged by a group that skips the sync.
    }
    size += WriteBatchInternal::ByteSize(*w->batch) - WriteBatchInternal::kHeaderSize;
    if (size > max_size) {
      break;
    }
    if (result == first->batch) {
      // Start merging in tmp_batch_ rather than modifying the caller's batch.
      tmp_batch_.Clear();
      WriteBatchInternal::Append(&tmp_batch_, *first->batch);
      result = &tmp_batch_;
    }
    WriteBatchInternal::Append(&tmp_batch_, *w->batch);
    *last_writer = w;
  }
  return result;
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
  // Without automatic compaction nothing would ever shrink level 0, so the
  // level-0 triggers would stop writes for good; they apply only with it.
  const bool level0_triggers = !options_.disable_auto_compactions;
  bool allow_delay = !force;
  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }
    if (allow_delay && level0_triggers &&
        versions_->NumLevelFiles(0) >= options_.level0_slowdown_writes_trigger) {
      // Level 0 is close to the stop trigger. Delaying each write group by
      // about a millisecond gives the CPU and disk to compaction and spreads
      // the cost over many writes, instead of stopping one write for as long
      // as a whole compaction takes. A write is delayed at most once.
      ++stats_.write_slowdowns;
      RecordStatistic(&Statistics::write_slowdowns);
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      lock.lock();
      allow_delay = false;
      continue;
    }
    if (force ? mem_->Empty() : mem_->BytesAdded() <= options_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      // The previous memtable is still being flushed. Wait instead of letting
      // memory grow without bound: this is the write path's backpressure.
      ++stats_.write_stalls;
      bg_done_cv_.wait(lock);
      continue;
    }
    if (level0_triggers && versions_->NumLevelFiles(0) >= options_.level0_stop_writes_trigger) {
      // Another level-0 file would make lookups probe too many files. Wait
      // for compaction (always pending here: stop >= compaction trigger).
      ++stats_.write_stops;
      RecordStatistic(&Statistics::write_stops);
      ++stopped_writers_;
      bg_done_cv_.notify_all();  // For TEST_WaitForStoppedWriter.
      bg_done_cv_.wait(lock);
      --stopped_writers_;
      continue;
    }
    return SwitchMemTable(lock);
  }
}

Status DBImpl::SwitchMemTable(std::unique_lock<std::mutex>& lock) {
  assert(imm_ == nullptr);
  const uint64_t new_log_number = versions_->NewFileNumber();
  log::Writer* const old_log = log_.get();
  std::unique_ptr<WritableFile> file;

  lock.unlock();
  // Recovery replays logs in order. Syncing the retiring log first means a
  // newer log is never durable while part of an older one is not, so replay
  // can never apply later writes across a gap.
  Status s = old_log->Sync();
  const bool old_log_sync_failed = !s.ok();
  if (s.ok()) {
    s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &file);
  }
  if (s.ok()) {
    // The new log's directory entry must be durable before a sync write to
    // it is acknowledged.
    s = env_->SyncDir(dbname_);
  }
  lock.lock();

  if (!s.ok()) {
    if (file != nullptr) {
      file.reset();
      static_cast<void>(env_->RemoveFile(LogFileName(dbname_, new_log_number)));
    }
    versions_->ReuseFileNumber(new_log_number);
    if (old_log_sync_failed) {
      // Whether the old log's contents reached the disk is unknown.
      RecordBackgroundError(s);
    }
    return s;
  }

  // The old log was just synced, so a close error cannot lose anything.
  static_cast<void>(log_->Close());
  log_ = std::make_unique<log::Writer>(std::move(file));
  logfile_number_ = new_log_number;
  imm_ = std::move(mem_);
  has_imm_.store(true, std::memory_order_release);
  mem_ = std::make_shared<MemTable>(internal_comparator_);
  bg_work_cv_.notify_one();
  return Status::OK();
}

void DBImpl::RecordBackgroundError(const Status& s) {
  if (bg_error_.ok()) {
    bg_error_ = s;
    bg_done_cv_.notify_all();
  }
}

void DBImpl::RecordStatistic(std::atomic<uint64_t> Statistics::*counter, uint64_t n) const {
  if (options_.statistics != nullptr) {
    ((*options_.statistics).*counter).fetch_add(n, std::memory_order_relaxed);
  }
}

bool DBImpl::BackgroundWorkPending() const {
  return imm_ != nullptr || manual_compaction_ != nullptr ||
         (!options_.disable_auto_compactions && !auto_compactions_paused_ &&
          versions_->NeedsCompaction());
}

void DBImpl::WaitForBackgroundIdle(std::unique_lock<std::mutex>& lock) {
  bg_done_cv_.wait(lock, [this] {
    return !bg_running_ && (!BackgroundWorkPending() || bg_paused_ || !bg_error_.ok() ||
                            shutting_down_.load(std::memory_order_relaxed));
  });
}

void DBImpl::BackgroundMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    bg_work_cv_.wait(lock, [this] {
      return shutting_down_.load(std::memory_order_relaxed) ||
             (!bg_paused_ && bg_error_.ok() && BackgroundWorkPending());
    });
    if (shutting_down_.load(std::memory_order_relaxed)) {
      break;
    }
    bg_running_ = true;
    BackgroundWork(lock);
    bg_running_ = false;
    bg_done_cv_.notify_all();
  }
}

void DBImpl::BackgroundWork(std::unique_lock<std::mutex>& lock) {
  if (imm_ != nullptr) {
    FlushImmutableMemTable(lock);
    return;
  }

  ManualCompaction* const manual = manual_compaction_;
  std::unique_ptr<Compaction> c;
  if (manual != nullptr) {
    manual->in_progress = true;
    c = versions_->CompactRange(manual->level, manual->begin, manual->end);
  } else if (!options_.disable_auto_compactions && !auto_compactions_paused_) {
    c = versions_->PickCompaction();
  }

  Status s;
  bool abandoned = false;
  if (c == nullptr) {
    // Nothing to do (a manual compaction of an empty range).
  } else if (manual == nullptr && c->IsTrivialMove()) {
    // Nothing in the next level overlaps the file, so it can move down by a
    // MANIFEST edit alone. A manual compaction always rewrites, because its
    // point is to drop overwritten and deleted entries.
    const FileMetaData& file = *c->inputs(0)[0];
    VersionEdit edit;
    edit.RemoveFile(c->level(), file.number);
    edit.AddFile(c->output_level(), file);
    s = versions_->LogAndApply(&edit, &lock);
    if (s.ok()) {
      ++stats_.levels[c->output_level()].trivial_moves;
    }
  } else {
    CompactionState compact(c.get());
    s = DoCompactionWork(&compact, lock);
    abandoned = compact.abandoned;
  }
  // Releases the compaction's reference to its input version, so the input
  // files become garbage once no reader holds that version either.
  c.reset();

  if (!s.ok() && !abandoned) {
    // Also while closing: a failed MANIFEST update may still have reached
    // the disk, and the sticky error is what keeps RemoveObsoleteFiles from
    // deleting files that the on-disk MANIFEST may list.
    RecordBackgroundError(s);
  }
  RemoveObsoleteFiles(lock);
  if (manual != nullptr) {
    // Completed only after the cleanup, so that CompactRange returns with
    // the replaced files already gone (unless a reader still uses them).
    manual->status = s;
    manual->done = true;
    manual_compaction_ = nullptr;
  }
}

void DBImpl::FlushImmutableMemTable(std::unique_lock<std::mutex>& lock) {
  assert(imm_ != nullptr);
  const std::shared_ptr<MemTable> imm = imm_;
  VersionEdit edit;
  uint64_t table_number = 0;
  Status s = WriteLevel0Table(imm.get(), &edit, lock, &table_number);
  if (s.ok()) {
    // Every write in the logs before the current one is now in a table.
    edit.SetLogNumber(logfile_number_);
    s = versions_->LogAndApply(&edit, &lock);
  }
  pending_outputs_.erase(table_number);
  if (s.ok()) {
    imm_.reset();
    has_imm_.store(false, std::memory_order_release);
    RemoveObsoleteFiles(lock);
  } else {
    RecordBackgroundError(s);
  }
}

Status DBImpl::DoCompactionWork(CompactionState* compact, std::unique_lock<std::mutex>& lock) {
  Compaction* const c = compact->compaction;
  const Comparator* const ucmp = internal_comparator_.user_comparator();
  const uint64_t throttle_us = options_.compaction_throttle_us;

  lock.unlock();
  // The compaction holds its input version, so the input files stay put
  // while they are read without the mutex.
  std::unique_ptr<Iterator> input = versions_->MakeInputIterator(*c);
  input->SeekToFirst();

  Status s;
  std::string current_user_key;
  bool has_current_user_key = false;
  uint64_t output_bytes_seen = 0;  // Output size when the throttle last paused.
  while (input->Valid()) {
    if (shutting_down_.load(std::memory_order_acquire)) {
      s = Status::IOError("compaction abandoned: database is closing");
      compact->abandoned = true;
      break;
    }
    if (has_imm_.load(std::memory_order_acquire)) {
      // A full memtable is waiting and writers may be blocked behind it.
      // Flushing it now adds a level-0 file newer than everything this
      // compaction reads, so the merge below is unaffected.
      lock.lock();
      if (imm_ != nullptr && bg_error_.ok()) {
        FlushImmutableMemTable(lock);
        bg_done_cv_.notify_all();
      }
      s = bg_error_;
      lock.unlock();
      if (!s.ok()) {
        break;
      }
    }

    const std::string_view key = input->key();
    ParsedInternalKey ikey;
    if (!ParseInternalKey(key, &ikey)) {
      s = Status::Corruption("malformed internal key in compaction input");
      break;
    }

    bool drop = false;
    if (!has_current_user_key || ucmp->Compare(ikey.user_key, current_user_key) != 0) {
      // The first, and so newest, version of this user key. Inputs from the
      // next level are older than inputs from this level, and every older
      // version of the key is either among the inputs or in a deeper level.
      current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
      has_current_user_key = true;
      // Output files are only cut between user keys, so a user key never
      // spans two files of a level >= 1 (their ranges stay disjoint).
      const bool stop_before = c->ShouldStopBefore(key);
      if (compact->builder != nullptr &&
          (stop_before || compact->builder->FileSize() >= c->MaxOutputFileSize())) {
        s = FinishCompactionOutputFile(compact);
        if (!s.ok()) {
          break;
        }
      }
      // A tombstone must stay while a deeper level may still hold an older
      // value it hides; below the deepest such level it has nothing to hide.
      drop = ikey.type == kTypeDeletion && c->IsBaseLevelForKey(ikey.user_key);
    } else {
      // An older version of a key seen just before. Every reader that could
      // still want it holds a version that references the input files, so
      // those readers keep seeing it there.
      drop = true;
    }

    if (!drop) {
      if (compact->builder == nullptr) {
        s = OpenCompactionOutputFile(compact);
        if (!s.ok()) {
          break;
        }
      }
      FileMetaData& output = compact->outputs.back();
      if (compact->builder->NumEntries() == 0) {
        output.smallest.DecodeFrom(key);
      }
      output.largest.DecodeFrom(key);
      compact->builder->Add(key, input->value());
      if (throttle_us > 0 && compact->builder->FileSize() != output_bytes_seen) {
        // The builder just wrote a data block.
        output_bytes_seen = compact->builder->FileSize();
        std::this_thread::sleep_for(std::chrono::microseconds(throttle_us));
      }
    }
    input->Next();
  }

  if (s.ok()) {
    s = input->status();
  }
  if (s.ok() && compact->builder != nullptr) {
    s = FinishCompactionOutputFile(compact);
  }
  if (compact->builder != nullptr) {
    compact->builder->Abandon();
    compact->builder.reset();
    compact->outfile.reset();
  }
  input.reset();

  lock.lock();
  bool install_attempted = false;
  if (s.ok()) {
    install_attempted = true;
    s = InstallCompactionResults(compact, lock);
  }
  if (!s.ok() && !install_attempted) {
    // Nothing refers to the outputs; remove them now rather than leaving
    // them for the next Open.
    for (const FileMetaData& output : compact->outputs) {
      DiscardTableFile(dbname_, env_, table_cache_.get(), output.number);
    }
  }
  for (const FileMetaData& output : compact->outputs) {
    pending_outputs_.erase(output.number);
  }
  return s;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact->builder == nullptr);
  uint64_t number = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    number = versions_->NewFileNumber();
    // Shield the file from RemoveObsoleteFiles until the MANIFEST lists it.
    pending_outputs_.insert(number);
  }
  FileMetaData output;
  output.number = number;
  compact->outputs.push_back(output);
  Status s = env_->NewWritableFile(TempFileName(dbname_, number), &compact->outfile);
  if (s.ok()) {
    compact->builder =
        std::make_unique<TableBuilder>(options_, &internal_comparator_, compact->outfile.get());
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact) {
  assert(compact->builder != nullptr);
  Status s = compact->builder->Finish();
  const uint64_t file_size = compact->builder->FileSize();
  compact->builder.reset();
  FileMetaData& output = compact->outputs.back();
  if (s.ok()) {
    s = InstallTableFile(dbname_, env_, table_cache_.get(), output.number, file_size,
                         std::move(compact->outfile));
  }
  compact->outfile.reset();
  if (s.ok()) {
    output.file_size = file_size;
    compact->total_bytes += file_size;
  }
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact,
                                        std::unique_lock<std::mutex>& lock) {
  const Compaction& c = *compact->compaction;
  VersionEdit edit;
  c.AddInputDeletions(&edit);
  for (const FileMetaData& output : compact->outputs) {
    edit.AddFile(c.output_level(), output);
  }
  // Inputs out, outputs in, as one MANIFEST record: after a crash the
  // database has either all of the old files or all of the new ones.
  Status s = versions_->LogAndApply(&edit, &lock);
  if (s.ok()) {
    const uint64_t bytes_read = c.TotalInputBytes();
    LevelStats& stats = stats_.levels[c.output_level()];
    ++stats.compactions;
    stats.bytes_read += bytes_read;
    stats.bytes_written += compact->total_bytes;
    RecordStatistic(&Statistics::compactions);
    RecordStatistic(&Statistics::compaction_bytes_read, bytes_read);
    RecordStatistic(&Statistics::compaction_bytes_written, compact->total_bytes);
  }
  return s;
}

Status DBImpl::CompactRange(const std::string* begin, const std::string* end) {
  Status s = Flush();
  if (!s.ok()) {
    return s;
  }
  // Compact level by level down to the deepest level holding part of the
  // range; below that there is nothing older to merge with.
  int max_level_with_files = 1;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    const std::shared_ptr<Version> base = versions_->current();
    std::string_view begin_key;
    std::string_view end_key;
    if (begin != nullptr) {
      begin_key = *begin;
    }
    if (end != nullptr) {
      end_key = *end;
    }
    for (int level = 1; level < kNumLevels; ++level) {
      if (base->OverlapInLevel(level, begin != nullptr ? &begin_key : nullptr,
                               end != nullptr ? &end_key : nullptr)) {
        max_level_with_files = level;
      }
    }
  }
  for (int level = 0; level < max_level_with_files; ++level) {
    s = RunManualCompaction(level, begin, end);
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}

Status DBImpl::RunManualCompaction(int level, const std::string* begin, const std::string* end) {
  assert(level >= 0 && level + 1 < kNumLevels);
  InternalKey begin_key;
  InternalKey end_key;
  ManualCompaction manual;
  manual.level = level;
  if (begin != nullptr) {
    // Sorts before every version of *begin.
    begin_key = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_key;
  }
  if (end != nullptr) {
    // Sorts after every version of *end.
    end_key = InternalKey(*end, 0, kTypeDeletion);
    manual.end = &end_key;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  // One manual compaction at a time.
  bg_done_cv_.wait(lock, [this] { return manual_compaction_ == nullptr || !bg_error_.ok(); });
  if (!bg_error_.ok()) {
    return bg_error_;
  }
  manual_compaction_ = &manual;
  bg_work_cv_.notify_all();
  // `manual` lives on this stack frame, so never return while the
  // background thread may still be using it.
  bg_done_cv_.wait(lock, [&] { return manual.done || (!bg_error_.ok() && !manual.in_progress); });
  if (!manual.done) {
    manual_compaction_ = nullptr;  // Failed before it started.
    return bg_error_;
  }
  return manual.status;
}

void DBImpl::WaitForBackgroundWork() {
  std::unique_lock<std::mutex> lock(mutex_);
  WaitForBackgroundIdle(lock);
}

void DBImpl::RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock) {
  if (!bg_error_.ok()) {
    // After a failed update it is unknown whether the MANIFEST change reached
    // the disk, so no file can safely be called garbage. Reopening resolves it.
    return;
  }
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  if (!env_->GetChildren(dbname_, &filenames).ok()) {
    return;  // Garbage is collected after the next flush instead.
  }
  std::vector<std::string> to_delete;
  for (const std::string& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (!ParseFileName(filename, &number, &type)) {
      continue;  // Not ours; never touched.
    }
    bool keep = true;
    switch (type) {
      case FileType::kLogFile:
        keep = number >= versions_->LogNumber();
        break;
      case FileType::kDescriptorFile:
        keep = number >= versions_->ManifestFileNumber();
        break;
      case FileType::kTableFile:
      case FileType::kTempFile:
        keep = live.contains(number);
        break;
      case FileType::kCurrentFile:
      case FileType::kLockFile:
        break;
    }
    if (!keep) {
      if (type == FileType::kTableFile) {
        table_cache_->Evict(number);
      }
      to_delete.push_back(dbname_ + "/" + filename);
    }
  }

  // Deleting can be slow; do it without the mutex. Only the background
  // thread (or Open, before that thread starts) gets here, so there is no
  // concurrent deleter.
  lock.unlock();
  for (const std::string& path : to_delete) {
    // A file that cannot be deleted now is retried after the next flush.
    static_cast<void>(env_->RemoveFile(path));
  }
  lock.lock();
}

Status DBImpl::Get(const ReadOptions& options, std::string_view key, std::string* value) {
  std::unique_lock<std::mutex> lock(mutex_);
  const SequenceNumber snapshot = versions_->LastSequence();
  const std::shared_ptr<MemTable> mem = mem_;
  const std::shared_ptr<MemTable> imm = imm_;
  const std::shared_ptr<Version> current = versions_->current();
  lock.unlock();

  // Newest data first: the active memtable, the one being flushed, then
  // the table files.
  const LookupKey lookup(key, snapshot);
  Status s;
  if (mem->Get(lookup, value, &s)) {
    return s;
  }
  if (imm != nullptr && imm->Get(lookup, value, &s)) {
    return s;
  }
  return current->Get(options, lookup, value);
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  std::unique_lock<std::mutex> lock(mutex_);
  const SequenceNumber snapshot = versions_->LastSequence();
  std::shared_ptr<MemTable> mem = mem_;
  std::shared_ptr<MemTable> imm = imm_;
  std::shared_ptr<Version> current = versions_->current();
  lock.unlock();

  // Newest source first, so that the merge breaks ties correctly.
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem->NewIterator());
  if (imm != nullptr) {
    children.push_back(imm->NewIterator());
  }
  current->AddIterators(options, &children);
  std::unique_ptr<Iterator> internal =
      NewMergingIterator(&internal_comparator_, std::move(children));

  // The iterator owns references to everything it reads from: the
  // memtables stay allocated and the version's table files undeleted until
  // it is destroyed.
  live_iterators_.fetch_add(1, std::memory_order_relaxed);
  internal->RegisterCleanup(
      [this, pinned_mem = std::move(mem), pinned_imm = std::move(imm),
       pinned_version = std::move(current)] {
        live_iterators_.fetch_sub(1, std::memory_order_relaxed);
      });
  return NewDBIterator(std::move(internal), internal_comparator_.user_comparator(), snapshot);
}

bool DBImpl::GetProperty(std::string_view property, std::string* value) {
  value->clear();
  constexpr std::string_view kPrefix = "kvlog.";
  if (!property.starts_with(kPrefix)) {
    return false;
  }
  property.remove_prefix(kPrefix.size());

  std::lock_guard<std::mutex> guard(mutex_);
  constexpr std::string_view kFilesAtLevel = "num-files-at-level";
  if (property.starts_with(kFilesAtLevel)) {
    property.remove_prefix(kFilesAtLevel.size());
    int level = -1;
    const char* const end = property.data() + property.size();
    const auto [ptr, ec] = std::from_chars(property.data(), end, level);
    if (ec != std::errc() || ptr != end || level < 0 || level >= kNumLevels) {
      return false;
    }
    *value = std::to_string(versions_->NumLevelFiles(level));
    return true;
  }
  if (property == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  }
  if (property == "approximate-memory-usage") {
    size_t total = mem_->ApproximateMemoryUsage();
    if (imm_ != nullptr) {
      total += imm_->ApproximateMemoryUsage();
    }
    *value = std::to_string(total);
    return true;
  }
  if (property == "stats") {
    char line[200];
    // Compactions are listed under the level they wrote to.
    value->append("Level  Files  Bytes         Compactions  Moves      Read(B)   Written(B)\n");
    for (int level = 0; level < kNumLevels; ++level) {
      const LevelStats& ls = stats_.levels[level];
      std::snprintf(line, sizeof(line), "%5d  %5d  %-12llu %12llu %6llu %12llu %12llu\n", level,
                    versions_->NumLevelFiles(level),
                    static_cast<unsigned long long>(versions_->NumLevelBytes(level)),
                    static_cast<unsigned long long>(ls.compactions),
                    static_cast<unsigned long long>(ls.trivial_moves),
                    static_cast<unsigned long long>(ls.bytes_read),
                    static_cast<unsigned long long>(ls.bytes_written));
      value->append(line);
    }
    std::snprintf(line, sizeof(line),
                  "flushes: %llu  flushed bytes: %llu  write stalls: %llu  "
                  "slowdowns: %llu  stops: %llu\n",
                  static_cast<unsigned long long>(stats_.flushes),
                  static_cast<unsigned long long>(stats_.flushed_bytes),
                  static_cast<unsigned long long>(stats_.write_stalls),
                  static_cast<unsigned long long>(stats_.write_slowdowns),
                  static_cast<unsigned long long>(stats_.write_stops));
    value->append(line);
    std::snprintf(line, sizeof(line),
                  "last sequence: %llu  log: %llu  manifest: %llu  batches recovered: %llu\n",
                  static_cast<unsigned long long>(versions_->LastSequence()),
                  static_cast<unsigned long long>(logfile_number_),
                  static_cast<unsigned long long>(versions_->ManifestFileNumber()),
                  static_cast<unsigned long long>(stats_.recovered_batches));
    value->append(line);
    if (!recovery_note_.empty()) {
      value->append("recovery: ").append(recovery_note_).append("\n");
    }
    if (!bg_error_.ok()) {
      value->append("background error: ").append(bg_error_.ToString()).append("\n");
    }
    if (const Cache* cache = table_cache_->block_cache(); cache != nullptr) {
      const Cache::Stats cache_stats = cache->GetStats();
      std::snprintf(line, sizeof(line),
                    "block cache: capacity %llu  usage %llu  hits %llu  misses %llu  "
                    "evictions %llu\n",
                    static_cast<unsigned long long>(cache->capacity()),
                    static_cast<unsigned long long>(cache->TotalCharge()),
                    static_cast<unsigned long long>(cache_stats.hits),
                    static_cast<unsigned long long>(cache_stats.misses),
                    static_cast<unsigned long long>(cache_stats.evictions));
      value->append(line);
    }
    if (options_.statistics != nullptr) {
      value->append("statistics:\n").append(options_.statistics->ToString());
    }
    return true;
  }
  return false;
}

void DBImpl::TEST_PauseBackgroundWork() {
  std::unique_lock<std::mutex> lock(mutex_);
  bg_paused_ = true;
  bg_done_cv_.wait(lock, [this] { return !bg_running_; });
}

void DBImpl::TEST_ResumeBackgroundWork() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    bg_paused_ = false;
  }
  bg_work_cv_.notify_all();
}

void DBImpl::TEST_WaitForBackgroundIdle() { WaitForBackgroundWork(); }

Status DBImpl::TEST_CompactRange(int level, const std::string* begin, const std::string* end) {
  return RunManualCompaction(level, begin, end);
}

void DBImpl::TEST_SetAutoCompactionsPaused(bool paused) {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto_compactions_paused_ = paused;
  }
  bg_work_cv_.notify_all();
}

void DBImpl::TEST_WaitForStoppedWriter() {
  std::unique_lock<std::mutex> lock(mutex_);
  bg_done_cv_.wait(lock, [this] { return stopped_writers_ > 0; });
}

std::shared_ptr<Version> DBImpl::TEST_CurrentVersion() {
  std::lock_guard<std::mutex> guard(mutex_);
  return versions_->current();
}

Status DBImpl::TEST_SwitchMemTable() {
  std::shared_ptr<MemTable> target;
  return WriteImpl(WriteOptions(), nullptr, &target);
}

bool DBImpl::TEST_HasImmutableMemTable() {
  std::lock_guard<std::mutex> guard(mutex_);
  return imm_ != nullptr;
}

uint64_t DBImpl::TEST_LogFileNumber() {
  std::lock_guard<std::mutex> guard(mutex_);
  return logfile_number_;
}

Status DBImpl::TEST_BackgroundError() {
  std::lock_guard<std::mutex> guard(mutex_);
  return bg_error_;
}

Status DestroyDB(const std::string& path, const Options& options) {
  Env* const env = options.env != nullptr ? options.env : Env::Default();
  std::vector<std::string> filenames;
  Status s = env->GetChildren(path, &filenames);
  if (!s.ok()) {
    return s.IsNotFound() ? Status::OK() : s;  // Nothing to destroy.
  }
  const std::string lock_name = LockFileName(path);
  std::unique_ptr<FileLock> lock;
  s = env->LockFile(lock_name, &lock);
  if (!s.ok()) {
    return s;
  }
  for (const std::string& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(filename, &number, &type) && type != FileType::kLockFile) {
      const Status removed = env->RemoveFile(path + "/" + filename);
      if (s.ok() && !removed.ok()) {
        s = removed;
      }
    }
  }
  // Logs that recovery set aside belong to the database too.
  const std::string lost_dir = path + "/" + kLostDirName;
  std::vector<std::string> lost_files;
  if (env->GetChildren(lost_dir, &lost_files).ok()) {
    for (const std::string& filename : lost_files) {
      static_cast<void>(env->RemoveFile(lost_dir + "/" + filename));
    }
    static_cast<void>(env->RemoveDir(lost_dir));
  }
  // Unlink LOCK while still holding it. Unlocking first would let another
  // process lock the old LOCK inode just before the unlink, and a third one
  // then lock a new LOCK file: two owners of one database.
  static_cast<void>(env->RemoveFile(lock_name));
  static_cast<void>(env->UnlockFile(std::move(lock)));
  // Fails, harmlessly, if the directory holds files kvlog did not create.
  static_cast<void>(env->RemoveDir(path));
  return s;
}

}  // namespace kvlog
