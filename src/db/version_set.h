#pragma once

// The set of live table files and its persistent record, the MANIFEST.
//
// A Version is an immutable snapshot of which table files make up the
// database, by level. Every flush (and later every compaction) produces a new
// Version by applying a VersionEdit to the current one. The edit is appended
// to the MANIFEST and synced before the new Version is installed, so the
// on-disk record never lags behind what readers can see, and a crash in the
// middle of an update leaves the previous state intact (the torn record is
// ignored on recovery).
//
// Versions are reference-counted with std::shared_ptr. A read takes a
// reference to the current version and keeps it until it is done, so the
// files it reads cannot be deleted under it: a table file is deleted only
// when no live version references it.
//
// The version set also chooses compactions (see compaction.h): every new
// version is scored, and the level most in need of compaction is recorded so
// the background thread can tell cheaply whether there is work to do.

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "db/compaction.h"
#include "db/dbformat.h"
#include "db/log_writer.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "kvlog/env.h"
#include "kvlog/iterator.h"
#include "kvlog/options.h"
#include "kvlog/status.h"

namespace kvlog {

class VersionSet;

class Version {
 public:
  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  // Looks `key` up in the table files: level 0 newest file first, then each
  // deeper level, stopping at the first file holding a value or tombstone
  // for the key. Returns NotFound if there is none (or the newest is a
  // tombstone).
  Status Get(const ReadOptions& options, const LookupKey& key, std::string* value) const;

  // Appends iterators that together cover every entry of this version: one
  // per level-0 file (newest first) and one concatenating iterator per
  // non-empty deeper level. The version must outlive them.
  void AddIterators(const ReadOptions& options,
                    std::vector<std::unique_ptr<Iterator>>* iters) const;

  int NumFiles(int level) const { return static_cast<int>(files_[level].size()); }
  const std::vector<std::shared_ptr<FileMetaData>>& files(int level) const {
    return files_[level];
  }

  // Stores in *inputs the files of `level` whose user-key range overlaps
  // [begin, end] (a null bound is unbounded). For level 0 the range grows to
  // cover every file found, transitively, because level-0 files overlap each
  // other and a key's versions may be spread over several of them.
  void GetOverlappingInputs(int level, const InternalKey* begin, const InternalKey* end,
                            std::vector<std::shared_ptr<FileMetaData>>* inputs) const;

  // True if some file of `level` overlaps the user-key range
  // [smallest, largest] (a null bound is unbounded).
  bool OverlapInLevel(int level, const std::string_view* smallest_user_key,
                      const std::string_view* largest_user_key) const;

  // How urgently this version needs a compaction (>= 1 means it does) and
  // of which level.
  double compaction_score() const { return compaction_score_; }
  int compaction_level() const { return compaction_level_; }

  std::string DebugString() const;

 private:
  friend class VersionSet;

  explicit Version(const VersionSet* vset) : vset_(vset) {}

  // An iterator over all files of `level` (>= 1), opening each table only
  // when the iteration reaches it.
  std::unique_ptr<Iterator> NewConcatenatingIterator(const ReadOptions& options, int level) const;

  const VersionSet* const vset_;

  // Set by VersionSet::Finalize before the version is installed.
  double compaction_score_ = -1;
  int compaction_level_ = -1;

  // Level 0 is ordered by file number, oldest first; flushes produce level-0
  // files in commit order, so a higher number means newer data. Deeper levels
  // are ordered by smallest key and their files never overlap.
  std::vector<std::shared_ptr<FileMetaData>> files_[kNumLevels];
};

// Owns the current Version, the MANIFEST writer and the counters that must
// survive restarts. Unless stated otherwise, every method requires the DB
// mutex to be held.
class VersionSet {
 public:
  // `options`, `table_cache` and `icmp` must outlive the version set.
  VersionSet(std::string dbname, const Options* options, TableCache* table_cache,
             const InternalKeyComparator* icmp);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;
  ~VersionSet();

  // Loads the MANIFEST named by CURRENT and installs the state it records.
  Status Recover();

  // Applies `edit` to the current version, appends the edit to the MANIFEST,
  // syncs it, and installs the result as the current version. Missing
  // fields of `edit` are filled in: the log number defaults to the current
  // one, and the next file number and last sequence are always set.
  //
  // `lock` must hold the DB mutex. It is released while the MANIFEST is
  // written and synced and re-acquired before returning. Callers guarantee
  // that only one LogAndApply runs at a time (the DB calls it only during
  // Open and from its single background thread).
  //
  // On failure the current version is unchanged. Whether the edit reached
  // the disk is then unknown, so the caller must stop deleting files.
  Status LogAndApply(VersionEdit* edit, std::unique_lock<std::mutex>* lock);

  std::shared_ptr<Version> current() const { return current_; }

  uint64_t NewFileNumber() { return next_file_number_++; }
  // Returns `number` to the pool if it was the last one allocated.
  void ReuseFileNumber(uint64_t number) {
    if (next_file_number_ == number + 1) {
      next_file_number_ = number;
    }
  }
  // Ensures future numbers are larger than `number`.
  void MarkFileNumberUsed(uint64_t number) {
    if (next_file_number_ <= number) {
      next_file_number_ = number + 1;
    }
  }

  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t LogNumber() const { return log_number_; }

  SequenceNumber LastSequence() const { return last_sequence_; }
  void SetLastSequence(SequenceNumber sequence);

  int NumLevelFiles(int level) const;
  uint64_t NumLevelBytes(int level) const;

  // Inserts the number of every table file referenced by a live version:
  // the current one or any older one still held by a reader.
  void AddLiveFiles(std::set<uint64_t>* live) const;

  // Number of files per level, e.g. "files[ 3 0 0 0 0 0 0 ]".
  std::string LevelSummary() const;

  // --- Compaction ---------------------------------------------------------

  // Size limit of level >= 1:
  // max_bytes_for_level_base * max_bytes_for_level_multiplier^(level-1).
  uint64_t MaxBytesForLevel(int level) const;

  // True if the current version has a level over its limit.
  bool NeedsCompaction() const { return current_->compaction_score_ >= 1; }

  // Chooses the compaction the current version needs most, or returns
  // nullptr if no level is over its limit.
  std::unique_ptr<Compaction> PickCompaction();

  // A compaction of the files of `level` that overlap [begin, end] (null
  // bounds are unbounded) into level + 1, or nullptr if there are none.
  // Requires: level + 1 < kNumLevels.
  std::unique_ptr<Compaction> CompactRange(int level, const InternalKey* begin,
                                           const InternalKey* end);

  // An iterator over every entry of the compaction's inputs, in internal
  // key order. Does not need the DB mutex: it reads only the compaction's
  // own file lists, which nothing modifies.
  std::unique_ptr<Iterator> MakeInputIterator(const Compaction& c) const;

 private:
  friend class Version;
  class Builder;

  // Computes the compaction score of `v`.
  void Finalize(Version* v) const;
  // Completes a compaction whose inputs at c->level() are chosen: adds the
  // overlapping files of the next level and the grandparents.
  void SetupOtherInputs(Compaction* c);

  // Writes the current version's full file list as one edit record.
  Status WriteSnapshot(log::Writer* log) const;
  void AppendVersion(std::shared_ptr<Version> version);

  Env* const env_;
  const std::string dbname_;
  const Options* const options_;
  TableCache* const table_cache_;
  const InternalKeyComparator* const icmp_;

  uint64_t next_file_number_ = 2;
  uint64_t manifest_file_number_ = 0;
  uint64_t log_number_ = 0;
  SequenceNumber last_sequence_ = 0;

  std::unique_ptr<log::Writer> descriptor_log_;  // nullptr until the first LogAndApply.
  std::shared_ptr<Version> current_;
  // Every version ever installed that may still be in use.
  std::vector<std::weak_ptr<Version>> live_versions_;

  // Per level, the largest internal key of the last compaction picked there;
  // the next one starts after it. Kept in memory only: after a restart the
  // rotation restarts from the beginning of the key space, which affects
  // only which file goes next, never correctness.
  std::string compact_pointer_[kNumLevels];
};

}  // namespace kvlog
