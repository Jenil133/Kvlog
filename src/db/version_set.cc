#include "db/version_set.h"

#include <algorithm>
#include <cassert>
#include <map>

#include "db/filename.h"
#include "table/merging_iterator.h"
#include "db/log_reader.h"
#include "table/iterator_util.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/env_util.h"

namespace kvlog {
namespace {

// Index of the first file whose largest key is >= `key`, or files.size().
size_t FindFile(const InternalKeyComparator& icmp,
                const std::vector<std::shared_ptr<FileMetaData>>& files, std::string_view key) {
  size_t left = 0;
  size_t right = files.size();
  while (left < right) {
    const size_t mid = left + (right - left) / 2;
    if (icmp.Compare(files[mid]->largest.Encode(), key) < 0) {
      left = mid + 1;  // Every file at or before mid ends before `key`.
    } else {
      right = mid;
    }
  }
  return right;
}

// Iterates over the files of one sorted level. key() is a file's largest
// internal key and value() its (number, size) as two fixed64s, which is what
// the table opener of a concatenating iterator needs.
class LevelFileNumIterator final : public Iterator {
 public:
  LevelFileNumIterator(const InternalKeyComparator& icmp,
                       const std::vector<std::shared_ptr<FileMetaData>>* files)
      : icmp_(icmp), files_(files), index_(files->size()) {}

  bool Valid() const override { return index_ < files_->size(); }
  void SeekToFirst() override { index_ = 0; }
  void Seek(std::string_view target) override { index_ = FindFile(icmp_, *files_, target); }
  void Next() override {
    assert(Valid());
    ++index_;
  }
  std::string_view key() const override {
    assert(Valid());
    return (*files_)[index_]->largest.Encode();
  }
  std::string_view value() const override {
    assert(Valid());
    EncodeFixed64(value_buf_, (*files_)[index_]->number);
    EncodeFixed64(value_buf_ + 8, (*files_)[index_]->file_size);
    return std::string_view(value_buf_, sizeof(value_buf_));
  }
  Status status() const override { return Status::OK(); }

 private:
  const InternalKeyComparator icmp_;
  const std::vector<std::shared_ptr<FileMetaData>>* const files_;
  size_t index_;
  mutable char value_buf_[16];
};

enum class LookupState { kNotFound, kFound, kDeleted, kCorrupt };

// Iterates over a sorted run of disjoint files (`files` must outlive the
// iterator), opening each table only when the iteration reaches it.
std::unique_ptr<Iterator> NewFileListIterator(
    const InternalKeyComparator& icmp, TableCache* table_cache, const ReadOptions& options,
    const std::vector<std::shared_ptr<FileMetaData>>* files) {
  return NewTwoLevelIterator(
      std::make_unique<LevelFileNumIterator>(icmp, files),
      [table_cache, options](std::string_view file_value) -> std::unique_ptr<Iterator> {
        if (file_value.size() != 16) {
          return NewErrorIterator(Status::Corruption("bad file entry in level iterator"));
        }
        return table_cache->NewIterator(options, DecodeFixed64(file_value.data()),
                                        DecodeFixed64(file_value.data() + 8));
      });
}

// The smallest and largest internal keys of `files` (not empty).
void GetRange(const InternalKeyComparator& icmp,
              const std::vector<std::shared_ptr<FileMetaData>>& files, InternalKey* smallest,
              InternalKey* largest) {
  assert(!files.empty());
  *smallest = files[0]->smallest;
  *largest = files[0]->largest;
  for (const auto& file : files) {
    if (icmp.Compare(file->smallest.Encode(), smallest->Encode()) < 0) {
      *smallest = file->smallest;
    }
    if (icmp.Compare(file->largest.Encode(), largest->Encode()) > 0) {
      *largest = file->largest;
    }
  }
}

}  // namespace

Status Version::Get(const ReadOptions& options, const LookupKey& key, std::string* value) const {
  const Comparator* const ucmp = vset_->icmp_->user_comparator();
  const std::string_view internal_key = key.internal_key();
  const std::string_view user_key = key.user_key();

  LookupState state = LookupState::kNotFound;
  const Table::EntryCallback on_entry = [&](std::string_view found_key,
                                            std::string_view found_value) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(found_key, &parsed)) {
      state = LookupState::kCorrupt;
      return;
    }
    if (ucmp->Compare(parsed.user_key, user_key) != 0) {
      return;  // The first entry at or after the key belongs to another key.
    }
    if (parsed.type == kTypeValue) {
      state = LookupState::kFound;
      value->assign(found_value.data(), found_value.size());
    } else {
      state = LookupState::kDeleted;
    }
  };

  // Searches one file; returns true once the lookup is decided.
  Status s;
  auto search = [&](const FileMetaData& file) {
    s = vset_->table_cache_->Get(options, file.number, file.file_size, internal_key, on_entry);
    return !s.ok() || state != LookupState::kNotFound;
  };
  auto finish = [&]() -> Status {
    if (!s.ok()) {
      return s;
    }
    switch (state) {
      case LookupState::kFound:
        return Status::OK();
      case LookupState::kCorrupt:
        return Status::Corruption("corrupted internal key in table", key.user_key());
      case LookupState::kNotFound:
      case LookupState::kDeleted:
        break;
    }
    return Status::NotFound(std::string_view());
  };

  // Level-0 files may overlap each other, so every file whose range covers
  // the key is a candidate; the newest one decides.
  for (auto it = files_[0].rbegin(); it != files_[0].rend(); ++it) {
    const FileMetaData& file = **it;
    if (ucmp->Compare(user_key, file.smallest.user_key()) < 0 ||
        ucmp->Compare(user_key, file.largest.user_key()) > 0) {
      continue;
    }
    if (search(file)) {
      return finish();
    }
  }

  // Deeper levels are sorted runs of disjoint files: at most one file per
  // level can hold the key, and any level holds older data than the ones
  // above it.
  for (int level = 1; level < kNumLevels; ++level) {
    const auto& files = files_[level];
    const size_t index = FindFile(*vset_->icmp_, files, internal_key);
    if (index == files.size()) {
      continue;
    }
    const FileMetaData& file = *files[index];
    if (ucmp->Compare(user_key, file.smallest.user_key()) < 0) {
      continue;
    }
    if (search(file)) {
      return finish();
    }
  }
  return finish();
}

std::unique_ptr<Iterator> Version::NewConcatenatingIterator(const ReadOptions& options,
                                                            int level) const {
  return NewFileListIterator(*vset_->icmp_, vset_->table_cache_, options, &files_[level]);
}

void Version::AddIterators(const ReadOptions& options,
                           std::vector<std::unique_ptr<Iterator>>* iters) const {
  for (auto it = files_[0].rbegin(); it != files_[0].rend(); ++it) {
    iters->push_back(vset_->table_cache_->NewIterator(options, (*it)->number, (*it)->file_size));
  }
  for (int level = 1; level < kNumLevels; ++level) {
    if (!files_[level].empty()) {
      iters->push_back(NewConcatenatingIterator(options, level));
    }
  }
}

void Version::GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end,
                                   std::vector<std::shared_ptr<FileMetaData>>* inputs) const {
  assert(level >= 0 && level < kNumLevels);
  inputs->clear();
  const Comparator* const ucmp = vset_->icmp_->user_comparator();
  std::string user_begin;
  std::string user_end;
  if (begin != nullptr) {
    user_begin = begin->user_key();
  }
  if (end != nullptr) {
    user_end = end->user_key();
  }
  const auto& files = files_[level];
  for (size_t i = 0; i < files.size();) {
    const auto& file = files[i++];
    const std::string_view file_start = file->smallest.user_key();
    const std::string_view file_limit = file->largest.user_key();
    if ((begin != nullptr && ucmp->Compare(file_limit, user_begin) < 0) ||
        (end != nullptr && ucmp->Compare(file_start, user_end) > 0)) {
      continue;  // Entirely outside the range.
    }
    inputs->push_back(file);
    if (level == 0) {
      // Level-0 files overlap each other: if this file reaches beyond the
      // range, widen the range and start over so that every level-0 file
      // holding versions of the same keys is included.
      if (begin != nullptr && ucmp->Compare(file_start, user_begin) < 0) {
        user_begin = file_start;
        inputs->clear();
        i = 0;
      } else if (end != nullptr && ucmp->Compare(file_limit, user_end) > 0) {
        user_end = file_limit;
        inputs->clear();
        i = 0;
      }
    }
  }
}

bool Version::OverlapInLevel(int level, const std::string_view* smallest_user_key,
                             const std::string_view* largest_user_key) const {
  const Comparator* const ucmp = vset_->icmp_->user_comparator();
  for (const auto& file : files_[level]) {
    if ((smallest_user_key != nullptr &&
         ucmp->Compare(file->largest.user_key(), *smallest_user_key) < 0) ||
        (largest_user_key != nullptr &&
         ucmp->Compare(file->smallest.user_key(), *largest_user_key) > 0)) {
      continue;
    }
    return true;
  }
  return false;
}

std::string Version::DebugString() const {
  std::string r;
  for (int level = 0; level < kNumLevels; ++level) {
    r.append("--- level ").append(std::to_string(level)).append(" ---\n");
    for (const auto& file : files_[level]) {
      r.append(" ").append(std::to_string(file->number)).append(":");
      r.append(std::to_string(file->file_size)).append("[");
      r.append(file->smallest.DebugString()).append(" .. ");
      r.append(file->largest.DebugString()).append("]\n");
    }
  }
  return r;
}

// Accumulates edits on top of a base version and produces the result.
class VersionSet::Builder {
 public:
  Builder(const InternalKeyComparator* icmp, const Version* base) : icmp_(icmp) {
    for (int level = 0; level < kNumLevels; ++level) {
      for (const auto& file : base->files_[level]) {
        levels_[level].emplace(file->number, file);
      }
    }
  }

  void Apply(const VersionEdit& edit) {
    for (const auto& [level, number] : edit.deleted_files_) {
      levels_[level].erase(number);
    }
    for (const auto& [level, file] : edit.new_files_) {
      levels_[level][file.number] = std::make_shared<FileMetaData>(file);
    }
  }

  // Fills `v` and checks the structural invariants: a file lives in at most
  // one level, and the files of each level >= 1 do not overlap.
  Status SaveTo(Version* v) const {
    std::set<uint64_t> seen;
    for (int level = 0; level < kNumLevels; ++level) {
      auto& files = v->files_[level];
      files.clear();
      for (const auto& [number, file] : levels_[level]) {
        if (!seen.insert(number).second) {
          return Status::Corruption("table file listed in two levels", std::to_string(number));
        }
        files.push_back(file);  // Ordered by file number.
      }
      if (level == 0) {
        continue;
      }
      std::sort(files.begin(), files.end(), [this](const auto& a, const auto& b) {
        return icmp_->Compare(a->smallest.Encode(), b->smallest.Encode()) < 0;
      });
      for (size_t i = 1; i < files.size(); ++i) {
        if (icmp_->Compare(files[i - 1]->largest.Encode(), files[i]->smallest.Encode()) >= 0) {
          return Status::Corruption("overlapping table files in level " + std::to_string(level));
        }
      }
    }
    return Status::OK();
  }

 private:
  const InternalKeyComparator* const icmp_;
  std::map<uint64_t, std::shared_ptr<FileMetaData>> levels_[kNumLevels];
};

VersionSet::VersionSet(std::string dbname, const Options* options, TableCache* table_cache,
                       const InternalKeyComparator* icmp)
    : env_(options->env),
      dbname_(std::move(dbname)),
      options_(options),
      table_cache_(table_cache),
      icmp_(icmp) {
  AppendVersion(std::shared_ptr<Version>(new Version(this)));
}

VersionSet::~VersionSet() {
  if (descriptor_log_ != nullptr) {
    // Every edit was synced when it was applied; nothing is lost if closing
    // reports an error.
    static_cast<void>(descriptor_log_->Close());
  }
}

void VersionSet::SetLastSequence(SequenceNumber sequence) {
  assert(sequence >= last_sequence_);
  last_sequence_ = sequence;
}

void VersionSet::AppendVersion(std::shared_ptr<Version> version) {
  Finalize(version.get());
  std::erase_if(live_versions_, [](const std::weak_ptr<Version>& v) { return v.expired(); });
  live_versions_.push_back(version);
  current_ = std::move(version);
}

Status VersionSet::Recover() {
  std::string current;
  Status s = ReadFileToString(env_, CurrentFileName(dbname_), &current);
  if (!s.ok()) {
    return s;
  }
  if (current.empty() || current.back() != '\n') {
    return Status::Corruption("CURRENT file does not end with a newline");
  }
  current.pop_back();
  uint64_t manifest_number = 0;
  FileType type;
  if (!ParseFileName(current, &manifest_number, &type) || type != FileType::kDescriptorFile) {
    return Status::Corruption("CURRENT does not name a MANIFEST", current);
  }

  const std::string manifest = dbname_ + "/" + current;
  std::unique_ptr<SequentialFile> file;
  s = env_->NewSequentialFile(manifest, &file);
  if (!s.ok()) {
    if (s.IsNotFound()) {
      return Status::Corruption("CURRENT names a missing MANIFEST", manifest);
    }
    return s;
  }

  bool have_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t log_number = 0;
  uint64_t next_file = 0;
  SequenceNumber last_sequence = 0;

  Builder builder(icmp_, current_.get());
  log::Reader reader(std::move(file));
  std::string record;
  while (reader.ReadRecord(&record)) {
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) {
      return Status::Corruption(manifest, s.message());
    }
    if (edit.has_comparator_ && edit.comparator_ != icmp_->user_comparator()->Name()) {
      return Status::InvalidArgument(
          "database was created with comparator " + edit.comparator_,
          std::string("this build uses ") + icmp_->user_comparator()->Name());
    }
    builder.Apply(edit);
    if (edit.has_log_number_) {
      log_number = edit.log_number_;
      have_log_number = true;
    }
    if (edit.has_next_file_number_) {
      next_file = edit.next_file_number_;
      have_next_file = true;
    }
    if (edit.has_last_sequence_) {
      last_sequence = edit.last_sequence_;
      have_last_sequence = true;
    }
  }
  switch (reader.state()) {
    case log::Reader::State::kEof:
    case log::Reader::State::kTornTail:
      // A torn final record is an edit whose LogAndApply never completed:
      // it was never installed, so the state before it is the right one.
      break;
    case log::Reader::State::kCorruption:
      return Status::Corruption(manifest, reader.status().message());
    case log::Reader::State::kIOError:
    case log::Reader::State::kReading:
      return reader.status();
  }
  if (!have_next_file || !have_log_number || !have_last_sequence) {
    return Status::Corruption(manifest, "missing next-file, log-number or last-sequence entry");
  }

  auto version = std::shared_ptr<Version>(new Version(this));
  s = builder.SaveTo(version.get());
  if (!s.ok()) {
    return Status::Corruption(manifest, s.message());
  }
  AppendVersion(std::move(version));
  manifest_file_number_ = manifest_number;
  next_file_number_ = next_file;
  log_number_ = log_number;
  last_sequence_ = last_sequence;
  MarkFileNumberUsed(manifest_number);
  MarkFileNumberUsed(log_number);
  return Status::OK();
}

Status VersionSet::WriteSnapshot(log::Writer* log) const {
  VersionEdit edit;
  edit.SetComparatorName(icmp_->user_comparator()->Name());
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& file : current_->files_[level]) {
      edit.AddFile(level, *file);
    }
  }
  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}

Status VersionSet::LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* lock) {
  assert(lock->owns_lock());
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }
  edit->SetLastSequence(last_sequence_);

  auto version = std::shared_ptr<Version>(new Version(this));
  {
    Builder builder(icmp_, current_.get());
    builder.Apply(*edit);
    Status s = builder.SaveTo(version.get());
    if (!s.ok()) {
      return s;
    }
  }

  // The first update after opening, and any update once the MANIFEST has
  // grown past the limit, starts a new MANIFEST that begins with a snapshot
  // of the current state. Older MANIFESTs then become garbage.
  Status s;
  std::unique_ptr<log::Writer> new_log;
  uint64_t new_manifest_number = 0;
  if (descriptor_log_ == nullptr || descriptor_log_->size() >= options_->max_manifest_file_size) {
    new_manifest_number = NewFileNumber();
    std::unique_ptr<WritableFile> file;
    s = env_->NewWritableFile(DescriptorFileName(dbname_, new_manifest_number), &file);
    if (s.ok()) {
      new_log = std::make_unique<log::Writer>(std::move(file));
      s = WriteSnapshot(new_log.get());
    }
  }
  edit->SetNextFile(next_file_number_);

  bool current_rename_attempted = false;
  if (s.ok()) {
    std::string record;
    edit->EncodeTo(&record);
    log::Writer* const log = new_log != nullptr ? new_log.get() : descriptor_log_.get();
    // Writing and syncing the MANIFEST is the slow part of every update;
    // release the mutex so foreground reads and writes are not stalled.
    // Nothing else modifies the version set meanwhile (see header).
    lock->unlock();
    s = log->AddRecord(record);
    if (s.ok()) {
      s = log->Sync();
    }
    if (s.ok() && new_log != nullptr) {
      s = SetCurrentFile(env_, dbname_, new_manifest_number, &current_rename_attempted);
    }
    lock->lock();
  }

  if (!s.ok()) {
    const bool appended_to_current_manifest = new_log == nullptr;
    if (new_log != nullptr) {
      static_cast<void>(new_log->Close());
      new_log.reset();
      // Once CURRENT may have been renamed to name the new MANIFEST, that
      // file could be the live one and must survive.
      if (!current_rename_attempted) {
        static_cast<void>(env_->RemoveFile(DescriptorFileName(dbname_, new_manifest_number)));
      }
    }
    // Stop using the open MANIFEST if the append may have left a partial
    // record at its end (never append after one), or if CURRENT may now name
    // the new MANIFEST, so that recovery would never read anything appended
    // here. Either way the next update starts a new MANIFEST and points
    // CURRENT at it.
    if ((appended_to_current_manifest || current_rename_attempted) && descriptor_log_ != nullptr) {
      static_cast<void>(descriptor_log_->Close());
      descriptor_log_.reset();
    }
    return s;
  }

  if (new_log != nullptr) {
    if (descriptor_log_ != nullptr) {
      static_cast<void>(descriptor_log_->Close());
    }
    descriptor_log_ = std::move(new_log);
    manifest_file_number_ = new_manifest_number;
  }
  log_number_ = edit->log_number_;
  AppendVersion(std::move(version));
  return Status::OK();
}

int VersionSet::NumLevelFiles(int level) const {
  assert(level >= 0 && level < kNumLevels);
  return current_->NumFiles(level);
}

uint64_t VersionSet::NumLevelBytes(int level) const {
  assert(level >= 0 && level < kNumLevels);
  uint64_t total = 0;
  for (const auto& file : current_->files_[level]) {
    total += file->file_size;
  }
  return total;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  for (const auto& weak : live_versions_) {
    if (const std::shared_ptr<Version> version = weak.lock()) {
      for (int level = 0; level < kNumLevels; ++level) {
        for (const auto& file : version->files_[level]) {
          live->insert(file->number);
        }
      }
    }
  }
}

std::string VersionSet::LevelSummary() const {
  std::string r = "files[";
  for (int level = 0; level < kNumLevels; ++level) {
    r.append(" ").append(std::to_string(current_->files_[level].size()));
  }
  r.append(" ]");
  return r;
}

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  assert(level >= 1);
  // Computed in floating point so that a large multiplier cannot overflow;
  // the result saturates far beyond any real level size.
  double bytes = static_cast<double>(options_->max_bytes_for_level_base);
  for (int i = 1; i < level; ++i) {
    bytes *= options_->max_bytes_for_level_multiplier;
  }
  constexpr double kMax = 9.0e18;
  return bytes >= kMax ? static_cast<uint64_t>(kMax) : static_cast<uint64_t>(bytes);
}

void VersionSet::Finalize(Version* v) const {
  // Level 0 is scored by file count rather than bytes: every level-0 file
  // may have to be consulted by a lookup, so the count is what hurts reads,
  // and small write buffers would otherwise compact far too often.
  double best_score =
      static_cast<double>(v->files_[0].size()) / options_->level0_compaction_trigger;
  int best_level = 0;
  // The last level has no level below it to compact into.
  for (int level = 1; level < kNumLevels - 1; ++level) {
    const double score = static_cast<double>(TotalFileSize(v->files_[level])) /
                         static_cast<double>(MaxBytesForLevel(level));
    if (score > best_score) {
      best_score = score;
      best_level = level;
    }
  }
  v->compaction_score_ = best_score;
  v->compaction_level_ = best_level;
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() {
  if (!NeedsCompaction()) {
    return nullptr;
  }
  const int level = current_->compaction_level_;
  assert(level >= 0 && level + 1 < kNumLevels);
  auto c = std::unique_ptr<Compaction>(new Compaction(*options_, icmp_, level, current_));

  const auto& files = current_->files_[level];
  assert(!files.empty());
  if (level == 0) {
    // The oldest file first: its data has waited longest, and the
    // overlapping-file expansion below pulls in whatever newer level-0
    // files hold versions of the same keys.
    c->inputs_[0].push_back(files[0]);
    InternalKey smallest;
    InternalKey largest;
    GetRange(*icmp_, c->inputs_[0], &smallest, &largest);
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
  } else {
    // The first file after the previous compaction of this level, wrapping
    // around at the end of the key space.
    for (const auto& file : files) {
      if (compact_pointer_[level].empty() ||
          icmp_->Compare(file->largest.Encode(), compact_pointer_[level]) > 0) {
        c->inputs_[0].push_back(file);
        break;
      }
    }
    if (c->inputs_[0].empty()) {
      c->inputs_[0].push_back(files[0]);
    }
  }
  SetupOtherInputs(c.get());
  return c;
}

std::unique_ptr<Compaction> VersionSet::CompactRange(int level, const InternalKey* begin,
                                                     const InternalKey* end) {
  assert(level >= 0 && level + 1 < kNumLevels);
  auto c = std::unique_ptr<Compaction>(new Compaction(*options_, icmp_, level, current_));
  current_->GetOverlappingInputs(level, begin, end, &c->inputs_[0]);
  if (c->inputs_[0].empty()) {
    return nullptr;
  }
  SetupOtherInputs(c.get());
  return c;
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();
  InternalKey smallest;
  InternalKey largest;
  GetRange(*icmp_, c->inputs_[0], &smallest, &largest);
  current_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);

  // The whole key range the compaction covers.
  InternalKey all_start = smallest;
  InternalKey all_limit = largest;
  if (!c->inputs_[1].empty()) {
    InternalKey next_smallest;
    InternalKey next_largest;
    GetRange(*icmp_, c->inputs_[1], &next_smallest, &next_largest);
    if (icmp_->Compare(next_smallest.Encode(), all_start.Encode()) < 0) {
      all_start = next_smallest;
    }
    if (icmp_->Compare(next_largest.Encode(), all_limit.Encode()) > 0) {
      all_limit = next_largest;
    }
  }
  if (level + 2 < kNumLevels) {
    current_->GetOverlappingInputs(level + 2, &all_start, &all_limit, &c->grandparents_);
  }

  // The next compaction of this level starts after this one's range. Doing
  // this when the compaction is picked (not when it commits) means a failed
  // compaction is not retried on the same files forever.
  compact_pointer_[level].assign(largest.Encode().data(), largest.Encode().size());
}

std::unique_ptr<Iterator> VersionSet::MakeInputIterator(const Compaction& c) const {
  const ReadOptions options;
  std::vector<std::unique_ptr<Iterator>> children;
  for (int which = 0; which < 2; ++which) {
    const auto& files = c.inputs(which);
    if (files.empty()) {
      continue;
    }
    if (c.level() + which == 0) {
      // Level-0 files overlap; each needs its own iterator. Newest first, the
      // order the merging iterator uses to break ties.
      for (auto it = files.rbegin(); it != files.rend(); ++it) {
        children.push_back(table_cache_->NewIterator(options, (*it)->number, (*it)->file_size));
      }
    } else {
      children.push_back(NewFileListIterator(*icmp_, table_cache_, options, &files));
    }
  }
  return NewMergingIterator(icmp_, std::move(children));
}

}  // namespace kvlog
