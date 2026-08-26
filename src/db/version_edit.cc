#include "db/version_edit.h"

#include "util/coding.h"

namespace kvlog {

// A version edit is a sequence of tagged fields, each a varint32 tag
// followed by the field's encoding, closed by kEndOfEdit. Tag numbers are
// part of the MANIFEST format: never reuse one. Tag 5 is reserved for
// compaction pointers.
enum Tag : uint32_t {
  kComparator = 1,      // length-prefixed name
  kLogNumber = 2,       // varint64
  kNextFileNumber = 3,  // varint64
  kLastSequence = 4,    // varint64
  kDeletedFile = 6,     // varint32 level, varint64 number
  kNewFile = 7,         // varint32 level, varint64 number, varint64 size,
                        // length-prefixed smallest and largest internal keys
  // Always the last byte of an encoded edit, and nothing follows it. Most
  // fields end in zero bytes (a varint of 0, the high bytes of a key's
  // fixed64 tag). The log reader treats a checksum failure in a final record
  // that ends in a run of zeros as possible zero fill from power loss; an
  // edit ending in this marker (one byte, 0x7f: seven bits set) never looks
  // like that, so damage to a committed final edit is always reported.
  kEndOfEdit = 127,
};

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixed(dst, comparator_);
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, file] : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, file.number);
    PutVarint64(dst, file.file_size);
    PutLengthPrefixed(dst, file.smallest.Encode());
    PutLengthPrefixed(dst, file.largest.Encode());
  }
  PutVarint32(dst, kEndOfEdit);
}

namespace {

bool GetLevel(std::string_view* input, int* level) {
  uint32_t value = 0;
  if (!GetVarint32(input, &value) || value >= static_cast<uint32_t>(kNumLevels)) {
    return false;
  }
  *level = static_cast<int>(value);
  return true;
}

bool GetInternalKey(std::string_view* input, InternalKey* key) {
  std::string_view encoded;
  ParsedInternalKey parsed;
  return GetLengthPrefixed(input, &encoded) && ParseInternalKey(encoded, &parsed) &&
         key->DecodeFrom(encoded);
}

}  // namespace

Status VersionEdit::DecodeFrom(std::string_view src) {
  Clear();
  std::string_view input = src;
  const char* error = nullptr;
  bool ended = false;
  while (error == nullptr && !ended && !input.empty()) {
    uint32_t tag = 0;
    if (!GetVarint32(&input, &tag)) {
      error = "bad tag";
      break;
    }
    switch (tag) {
      case kComparator: {
        std::string_view name;
        if (GetLengthPrefixed(&input, &name)) {
          SetComparatorName(name);
        } else {
          error = "bad comparator name";
        }
        break;
      }
      case kLogNumber:
        if (GetVarint64(&input, &log_number_)) {
          has_log_number_ = true;
        } else {
          error = "bad log number";
        }
        break;
      case kNextFileNumber:
        if (GetVarint64(&input, &next_file_number_)) {
          has_next_file_number_ = true;
        } else {
          error = "bad next file number";
        }
        break;
      case kLastSequence:
        if (GetVarint64(&input, &last_sequence_)) {
          has_last_sequence_ = true;
        } else {
          error = "bad last sequence number";
        }
        break;
      case kDeletedFile: {
        int level = 0;
        uint64_t number = 0;
        if (GetLevel(&input, &level) && GetVarint64(&input, &number)) {
          deleted_files_.emplace(level, number);
        } else {
          error = "bad deleted file entry";
        }
        break;
      }
      case kNewFile: {
        int level = 0;
        FileMetaData file;
        if (GetLevel(&input, &level) && GetVarint64(&input, &file.number) &&
            GetVarint64(&input, &file.file_size) && GetInternalKey(&input, &file.smallest) &&
            GetInternalKey(&input, &file.largest)) {
          new_files_.emplace_back(level, std::move(file));
        } else {
          error = "bad new file entry";
        }
        break;
      }
      case kEndOfEdit:
        ended = true;
        if (!input.empty()) {
          error = "data after the end of the edit";
        }
        break;
      default:
        error = "unknown tag";
        break;
    }
  }
  if (error == nullptr && !ended) {
    error = "missing end of edit";
  }
  if (error != nullptr) {
    return Status::Corruption("malformed version edit", error);
  }
  return Status::OK();
}

std::string VersionEdit::DebugString() const {
  std::string r = "VersionEdit {";
  if (has_comparator_) {
    r.append("\n  Comparator: ").append(comparator_);
  }
  if (has_log_number_) {
    r.append("\n  LogNumber: ").append(std::to_string(log_number_));
  }
  if (has_next_file_number_) {
    r.append("\n  NextFile: ").append(std::to_string(next_file_number_));
  }
  if (has_last_sequence_) {
    r.append("\n  LastSeq: ").append(std::to_string(last_sequence_));
  }
  for (const auto& [level, number] : deleted_files_) {
    r.append("\n  RemoveFile: ").append(std::to_string(level)).append(" ");
    r.append(std::to_string(number));
  }
  for (const auto& [level, file] : new_files_) {
    r.append("\n  AddFile: ").append(std::to_string(level)).append(" ");
    r.append(std::to_string(file.number)).append(" ").append(std::to_string(file.file_size));
    r.append(" [").append(file.smallest.DebugString()).append(" .. ");
    r.append(file.largest.DebugString()).append("]");
  }
  r.append("\n}\n");
  return r;
}

}  // namespace kvlog
