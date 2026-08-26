#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "db/dbformat.h"
#include "kvlog/status.h"

namespace kvlog {

// Metadata of one table file. Shared (via std::shared_ptr) by every Version
// that contains the file; its use count is the number of live versions (and
// in-flight readers) that can still reach it.
struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  InternalKey smallest;  // Smallest internal key in the table.
  InternalKey largest;   // Largest internal key in the table.
};

// A change to the database's metadata, appended to the MANIFEST as one log
// record: table files added and removed (each with its level), plus the
// counters needed to resume after a restart. Replaying the edits of a
// MANIFEST in order reconstructs the current state.
class VersionEdit {
 public:
  void Clear() { *this = VersionEdit(); }

  void SetComparatorName(std::string_view name) {
    has_comparator_ = true;
    comparator_.assign(name.data(), name.size());
  }
  // Write-ahead logs numbered below this hold only data already in tables.
  void SetLogNumber(uint64_t number) {
    has_log_number_ = true;
    log_number_ = number;
  }
  void SetNextFile(uint64_t number) {
    has_next_file_number_ = true;
    next_file_number_ = number;
  }
  void SetLastSequence(SequenceNumber sequence) {
    has_last_sequence_ = true;
    last_sequence_ = sequence;
  }

  void AddFile(int level, const FileMetaData& file) { new_files_.emplace_back(level, file); }
  void RemoveFile(int level, uint64_t number) { deleted_files_.emplace(level, number); }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);

  std::string DebugString() const;

 private:
  friend class VersionSet;

  std::string comparator_;
  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  bool has_comparator_ = false;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::set<std::pair<int, uint64_t>> deleted_files_;  // (level, file number)
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace kvlog
