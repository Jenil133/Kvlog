#pragma once

// One leveled compaction: the input files chosen from a level and the next
// one, plus the bookkeeping the merge loop needs to decide which entries to
// keep and where to cut output files.
//
// Compaction policy (LevelDB style):
//  - Level 0 is compacted into level 1 once it holds
//    Options::level0_compaction_trigger files. Level-0 files may overlap, so
//    the inputs are the oldest level-0 file plus every level-0 file whose key
//    range overlaps the growing input range.
//  - A level n >= 1 is compacted into level n + 1 once its total size
//    exceeds max_bytes_for_level_base * max_bytes_for_level_multiplier^(n-1).
//    One file is picked, round robin through the key space, so that over
//    time every part of the level gets compacted.
//  - The inputs always include every file of the next level that overlaps
//    them, so the output level stays a sorted run of disjoint files.
//  - The last level is never compacted; it is where data comes to rest.

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "db/dbformat.h"
#include "db/version_edit.h"
#include "kvlog/options.h"

namespace kvlog {

class Version;

class Compaction {
 public:
  Compaction(const Compaction&) = delete;
  Compaction& operator=(const Compaction&) = delete;
  ~Compaction();

  // Inputs come from level() (which = 0) and level() + 1 (which = 1);
  // outputs go to level() + 1.
  int level() const { return level_; }
  int output_level() const { return level_ + 1; }

  const std::vector<std::shared_ptr<FileMetaData>>& inputs(int which) const {
    return inputs_[which];
  }
  int num_input_files(int which) const { return static_cast<int>(inputs_[which].size()); }

  // The version the inputs were chosen from. Holding it keeps every input
  // file (and every file IsBaseLevelForKey looks at) from being deleted.
  const std::shared_ptr<Version>& input_version() const { return input_version_; }

  // Output files are cut once they reach this size.
  uint64_t MaxOutputFileSize() const { return max_output_file_size_; }

  // Bytes of all input files.
  uint64_t TotalInputBytes() const;

  // True if the compaction can simply move its single input file down one
  // level: nothing in the next level overlaps it, and moving it does not
  // create a file that overlaps too much of the level below (which would make
  // that file's own compaction expensive later).
  bool IsTrivialMove() const;

  // Records the removal of every input file in `edit`.
  void AddInputDeletions(VersionEdit* edit) const;

  // True if no level below the output level can contain `user_key`, so a
  // tombstone for it has nothing left to hide and can be dropped. Calls must
  // pass user keys in increasing order.
  bool IsBaseLevelForKey(std::string_view user_key);

  // True if the output file being built should be finished before
  // `internal_key` is added, because it already overlaps so much of the
  // level below the output that compacting it later would be expensive.
  bool ShouldStopBefore(std::string_view internal_key);

 private:
  friend class VersionSet;

  Compaction(const Options& options, const InternalKeyComparator* icmp, int level,
             std::shared_ptr<Version> input_version);

  const InternalKeyComparator* const icmp_;
  const int level_;
  const uint64_t max_output_file_size_;
  const uint64_t max_grandparent_overlap_bytes_;
  std::shared_ptr<Version> input_version_;

  std::vector<std::shared_ptr<FileMetaData>> inputs_[2];
  // Files of level() + 2 overlapping the compaction's key range.
  std::vector<std::shared_ptr<FileMetaData>> grandparents_;

  // ShouldStopBefore state: the first grandparent not yet passed, and the
  // bytes of grandparents the current output overlaps.
  size_t grandparent_index_ = 0;
  bool seen_key_ = false;
  uint64_t overlapped_bytes_ = 0;

  // IsBaseLevelForKey state: per level, the first file whose largest key is
  // not before the last key asked about. Keys arrive in order, so each level
  // is scanned once per compaction.
  size_t level_ptrs_[kNumLevels] = {};
};

// Total size of `files`.
uint64_t TotalFileSize(const std::vector<std::shared_ptr<FileMetaData>>& files);

}  // namespace kvlog
