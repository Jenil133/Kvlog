#pragma once

// The filter block of a table file: one Bloom filter (util/bloom.h) over the
// user keys of every entry in the table.
//
// One filter per table rather than one per data block: a point lookup asks
// the filter before it touches the index, so a negative answer costs no
// index search and no block read, and a table needs a single small block of
// metadata. A table of 2 MiB with 100-byte entries holds about 20k keys,
// which at 10 bits per key is a 25 KiB filter, loaded when the table opens.
//
// The block is stored like any other block (with a checksummed trailer)
// between the last data block and the metaindex block, and the metaindex maps
// kFilterBlockName to its handle. Tables written without a filter simply
// have no such entry, and readers then skip the filter check; names a reader
// does not know are ignored, so a future filter kind can sit next to this
// one.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/bloom.h"

namespace kvlog {

// Metaindex key of the filter block.
inline constexpr std::string_view kFilterBlockName = "filter.kvlog.bloom64";

// Collects the user keys of a table as it is built.
class FilterBlockBuilder {
 public:
  // `bits_per_key` must be positive.
  explicit FilterBlockBuilder(int bits_per_key) : bits_per_key_(bits_per_key) {}

  // Adds a user key. Tables hold every version of a user key next to each
  // other, so repeats of the previous key are skipped: they would only
  // dilute the filter.
  void AddKey(std::string_view user_key);

  // Returns the encoded filter. The builder is spent afterwards.
  std::string Finish();

  size_t NumKeys() const { return key_hashes_.size(); }

 private:
  const int bits_per_key_;
  std::vector<uint64_t> key_hashes_;
  std::string last_key_;
  bool has_last_key_ = false;
};

// Answers membership queries from an encoded filter block. The block must
// outlive the reader.
class FilterBlockReader {
 public:
  explicit FilterBlockReader(std::string_view contents) : filter_(contents) {}

  bool KeyMayMatch(std::string_view user_key) const { return filter_.KeyMayMatch(user_key); }

 private:
  BloomFilterReader filter_;
};

}  // namespace kvlog
