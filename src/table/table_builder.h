#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "kvlog/env.h"
#include "kvlog/options.h"
#include "kvlog/status.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/comparator.h"

namespace kvlog {

// Writes a table file (see format.h) from entries added in sorted order.
// The caller owns the file and is responsible for syncing and closing it
// after Finish(); see BuildTable() for the full crash-safe protocol.
//
// Keys are internal keys (db/dbformat.h). When options.bloom_bits_per_key is
// positive the table also gets a filter block over the user keys (see
// filter_block.h).
class TableBuilder {
 public:
  // Entries must be added in `comparator` order. `file` must stay open until
  // Finish() or Abandon() returns.
  TableBuilder(const Options& options, const Comparator* comparator, WritableFile* file);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;
  // Requires: Finish() or Abandon() has been called.
  ~TableBuilder();

  // `key` must sort after every key added before; a key that does not makes
  // the builder fail with InvalidArgument (see status() and Finish()), as
  // does a key too short to be an internal key when a filter is built.
  void Add(std::string_view key, std::string_view value);

  // Writes the last data block, the filter block (if any), the metaindex
  // and index blocks and the footer. Returns the first error encountered while building.
  Status Finish();

  // Stops building; the partial file must be discarded by the caller.
  void Abandon();

  Status status() const { return status_; }
  uint64_t NumEntries() const { return num_entries_; }
  // Bytes written to the file so far; the final file size after Finish().
  uint64_t FileSize() const { return offset_; }

 private:
  void FlushDataBlock();
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(std::string_view contents, BlockHandle* handle);

  const size_t block_size_;
  const Comparator* const comparator_;
  WritableFile* const file_;
  uint64_t offset_ = 0;
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::unique_ptr<FilterBlockBuilder> filter_block_;  // nullptr: no filter.
  std::string last_key_;
  uint64_t num_entries_ = 0;
  bool closed_ = false;
};

}  // namespace kvlog
