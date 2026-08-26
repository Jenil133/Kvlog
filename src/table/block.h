#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "kvlog/iterator.h"
#include "table/format.h"
#include "util/comparator.h"

namespace kvlog {

// A parsed, immutable block (see block_builder.h for the format). Every
// offset read from the block is bounds-checked, so a damaged block yields
// Corruption from its iterator rather than out-of-bounds reads.
class Block {
 public:
  explicit Block(BlockContents contents);
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return data_.size(); }

  // Returns an iterator over the block's entries ordered by `comparator`.
  // The block must outlive the iterator.
  std::unique_ptr<Iterator> NewIterator(const Comparator* comparator) const;

 private:
  class Iter;

  std::unique_ptr<char[]> heap_;
  std::string_view data_;
  uint32_t restart_offset_ = 0;  // Offset of the restart array in data_.
  uint32_t num_restarts_ = 0;
  bool malformed_ = false;
};

}  // namespace kvlog
