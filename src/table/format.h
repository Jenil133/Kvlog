#pragma once

// Table (".sst") file layout:
//
//   [data block 0] ... [data block N-1]
//   [filter block]            (optional; see filter_block.h)
//   [metaindex block]
//   [index block]
//   [footer]
//
// Every block is followed by a 5-byte trailer:
//
//   storage type (uint8, 0 = stored as-is) | masked CRC-32C (fixed32) of the
//   block contents followed by the storage type byte
//
// Data blocks hold internal keys and values in sorted order (see
// block_builder.h). The index block has one entry per data block, mapping
// the last internal key in that block to the block's BlockHandle. The
// metaindex block maps meta block names to BlockHandles; readers find meta
// blocks by name, so new kinds can be added without changing the footer or
// the format version. The only one so far is the Bloom filter block, whose
// entry is absent in tables written without a filter.
//
// The footer has a fixed size and sits at the very end of the file:
//
//   metaindex handle | index handle | zero padding up to 40 bytes |
//   format version (fixed32) | magic number (fixed64)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "kvlog/env.h"
#include "kvlog/status.h"
#include "util/coding.h"

namespace kvlog {

// Location of a block within a table file. `size` excludes the trailer.
class BlockHandle {
 public:
  static constexpr size_t kMaxEncodedLength = 2 * kMaxVarint64Length;

  BlockHandle() = default;
  BlockHandle(uint64_t offset, uint64_t size) : offset_(offset), size_(size) {}

  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view* input);

 private:
  uint64_t offset_ = 0;
  uint64_t size_ = 0;
};

class Footer {
 public:
  static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 4 + 8;

  Footer() = default;
  Footer(const BlockHandle& metaindex, const BlockHandle& index)
      : metaindex_handle_(metaindex), index_handle_(index) {}

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  const BlockHandle& index_handle() const { return index_handle_; }

  void EncodeTo(std::string* dst) const;
  // `input` must be exactly kEncodedLength bytes.
  Status DecodeFrom(std::string_view input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// The ASCII bytes "KVLOGTB1" read as a big-endian integer.
inline constexpr uint64_t kTableMagicNumber = 0x4b564c4f47544231ull;

// Bumped whenever the table layout changes incompatibly.
inline constexpr uint32_t kTableFormatVersion = 1;

inline constexpr size_t kBlockTrailerSize = 5;

enum BlockStorage : uint8_t {
  kRawBlock = 0,
};

// The verified contents of one block, without the trailer.
struct BlockContents {
  std::unique_ptr<char[]> heap;  // Owns the bytes `data` points into.
  std::string_view data;
};

// Reads the block at `handle` from a table file of `file_size` bytes and
// verifies its checksum. Fails with Corruption if the handle lies outside
// the file, the read comes up short, or the checksum does not match.
Status ReadBlock(const RandomAccessFile& file, uint64_t file_size, const BlockHandle& handle,
                 BlockContents* result);

}  // namespace kvlog
