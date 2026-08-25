#pragma once

// Internal keys. Every entry stored in a memtable or table is keyed by
//
//   internal_key := user_key | tag (fixed64, little-endian)
//   tag          := (sequence << 8) | value_type
//
// Internal keys sort by user key ascending, then by sequence number
// descending, so for any user key the newest version is encountered first.
// Sequence numbers are assigned to writes in commit order and never reused;
// a read at sequence S sees exactly the writes numbered <= S.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "util/coding.h"
#include "util/comparator.h"

namespace kvlog {

// Number of levels in the LSM tree. Flushes produce level-0 files, whose key
// ranges may overlap; every deeper level is a sorted run of non-overlapping
// files. The MANIFEST records each file's level.
inline constexpr int kNumLevels = 7;

using SequenceNumber = uint64_t;

// Stored in the low byte of the tag, so the values are part of the on-disk
// format and must never be renumbered.
enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1,
};

// Entries with equal user key and sequence sort by decreasing type, so a seek
// target built with the largest type lands before all of them.
inline constexpr ValueType kValueTypeForSeek = kTypeValue;

// Sequence numbers fill the upper 56 bits of the tag.
inline constexpr SequenceNumber kMaxSequenceNumber = (SequenceNumber{1} << 56) - 1;

inline constexpr size_t kTagSize = 8;

inline uint64_t PackSequenceAndType(SequenceNumber sequence, ValueType type) {
  assert(sequence <= kMaxSequenceNumber);
  return (sequence << 8) | type;
}

struct ParsedInternalKey {
  ParsedInternalKey() = default;
  ParsedInternalKey(std::string_view user_key_in, SequenceNumber sequence_in, ValueType type_in)
      : user_key(user_key_in), sequence(sequence_in), type(type_in) {}

  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = kTypeDeletion;
};

void AppendInternalKey(std::string* result, const ParsedInternalKey& key);

// Returns false if `internal_key` is shorter than a tag or has an unknown
// value type.
bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result);

inline std::string_view ExtractUserKey(std::string_view internal_key) {
  assert(internal_key.size() >= kTagSize);
  return internal_key.substr(0, internal_key.size() - kTagSize);
}

inline uint64_t ExtractTag(std::string_view internal_key) {
  assert(internal_key.size() >= kTagSize);
  return DecodeFixed64(internal_key.data() + internal_key.size() - kTagSize);
}

// Orders internal keys: user key ascending (by the user comparator), then
// sequence number descending, then type descending.
class InternalKeyComparator final : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator* user_comparator)
      : user_comparator_(user_comparator) {}

  int Compare(std::string_view a, std::string_view b) const override;
  const char* Name() const override { return "kvlog.InternalKeyComparator"; }

  const Comparator* user_comparator() const { return user_comparator_; }

 private:
  const Comparator* user_comparator_;
};

// An owned, encoded internal key. Used for file boundaries in the version
// metadata, where keys must outlive the buffers they were read from.
class InternalKey {
 public:
  InternalKey() = default;  // Empty: not a valid key until assigned.
  InternalKey(std::string_view user_key, SequenceNumber sequence, ValueType type) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, sequence, type));
  }

  // Returns false if `encoded` is empty.
  bool DecodeFrom(std::string_view encoded) {
    rep_.assign(encoded.data(), encoded.size());
    return !rep_.empty();
  }

  std::string_view Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  std::string_view user_key() const { return ExtractUserKey(rep_); }

  bool empty() const { return rep_.empty(); }
  void Clear() { rep_.clear(); }

  std::string DebugString() const;

 private:
  std::string rep_;
};

// The key used to look up `user_key` as of `sequence` in a memtable or
// table. It encodes the memtable entry prefix
//
//   varint32(internal_key.size()) | user_key | tag(sequence, kValueTypeForSeek)
//
// so the same buffer serves as both a memtable key and an internal key.
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber sequence);
  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  std::string_view memtable_key() const {
    return std::string_view(start_, static_cast<size_t>(end_ - start_));
  }
  std::string_view internal_key() const {
    return std::string_view(kstart_, static_cast<size_t>(end_ - kstart_));
  }
  std::string_view user_key() const {
    return std::string_view(kstart_, static_cast<size_t>(end_ - kstart_) - kTagSize);
  }

 private:
  const char* start_;
  const char* kstart_;
  const char* end_;
  // Short keys (the common case) are encoded here without a heap allocation.
  char inline_[200];
  std::unique_ptr<char[]> heap_;
};

}  // namespace kvlog
