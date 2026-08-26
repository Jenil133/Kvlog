#include "db/compaction.h"

#include "db/version_set.h"

namespace kvlog {
namespace {

// How much of the level below its output one compaction output file may
// overlap. Bounds the cost of compacting that file later.
uint64_t MaxGrandparentOverlapBytes(const Options& options) {
  return 10 * options.target_file_size;
}

}  // namespace

uint64_t TotalFileSize(const std::vector<std::shared_ptr<FileMetaData>>& files) {
  uint64_t total = 0;
  for (const auto& file : files) {
    total += file->file_size;
  }
  return total;
}

Compaction::Compaction(const Options& options, const InternalKeyComparator* icmp, int level,
                       std::shared_ptr<Version> input_version)
    : icmp_(icmp),
      level_(level),
      max_output_file_size_(options.target_file_size),
      max_grandparent_overlap_bytes_(MaxGrandparentOverlapBytes(options)),
      input_version_(std::move(input_version)) {}

Compaction::~Compaction() = default;

uint64_t Compaction::TotalInputBytes() const {
  return TotalFileSize(inputs_[0]) + TotalFileSize(inputs_[1]);
}

bool Compaction::IsTrivialMove() const {
  return num_input_files(0) == 1 && num_input_files(1) == 0 &&
         TotalFileSize(grandparents_) <= max_grandparent_overlap_bytes_;
}

void Compaction::AddInputDeletions(VersionEdit* edit) const {
  for (int which = 0; which < 2; ++which) {
    for (const auto& file : inputs_[which]) {
      edit->RemoveFile(level_ + which, file->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(std::string_view user_key) {
  const Comparator* const ucmp = icmp_->user_comparator();
  for (int level = level_ + 2; level < kNumLevels; ++level) {
    const auto& files = input_version_->files(level);
    size_t& index = level_ptrs_[level];
    while (index < files.size()) {
      const FileMetaData& file = *files[index];
      if (ucmp->Compare(user_key, file.largest.user_key()) <= 0) {
        if (ucmp->Compare(user_key, file.smallest.user_key()) >= 0) {
          return false;  // This file's range covers the key.
        }
        break;  // The key falls in the gap before this file.
      }
      ++index;  // Files ending before the key are behind every later key too.
    }
  }
  return true;
}

bool Compaction::ShouldStopBefore(std::string_view internal_key) {
  while (grandparent_index_ < grandparents_.size() &&
         icmp_->Compare(internal_key, grandparents_[grandparent_index_]->largest.Encode()) > 0) {
    if (seen_key_) {
      overlapped_bytes_ += grandparents_[grandparent_index_]->file_size;
    }
    ++grandparent_index_;
  }
  seen_key_ = true;
  if (overlapped_bytes_ > max_grandparent_overlap_bytes_) {
    overlapped_bytes_ = 0;
    return true;
  }
  return false;
}

}  // namespace kvlog
