#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "kvlog/env.h"
#include "kvlog/iterator.h"
#include "kvlog/options.h"
#include "kvlog/status.h"
#include "kvlog/statistics.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/cache.h"
#include "util/comparator.h"

namespace kvlog {

// An open, immutable table file. Thread-safe: any number of threads may read
// through one Table concurrently.
//
// Opening a table reads and verifies the footer and keeps the index block and
// the filter block (if the table has one) in memory. Data blocks are read on
// demand, each through ReadDataBlock().
//
// Point lookups (InternalGet) ask the filter first, then search the index,
// then read one data block, through the block cache when the table has one.
// Iterators read data blocks directly and leave the cache alone: a scan, and
// in particular a compaction, touches every block once, and letting it fill
// the cache would evict the blocks that repeated point lookups need.
class Table {
 public:
  using EntryCallback = std::function<void(std::string_view key, std::string_view value)>;

  // Opens the table stored in `file` (`file_size` bytes). `file_number`
  // identifies the table in error messages and, with the block offset, keys
  // its blocks in `block_cache` (optional), so tables sharing a cache must
  // have distinct numbers. `comparator` orders the keys and must outlive the
  // table. Lookups update options.statistics, if set.
  static Status Open(const Options& options, const Comparator* comparator,
                     std::unique_ptr<RandomAccessFile> file, uint64_t file_number,
                     uint64_t file_size, std::shared_ptr<Table>* table,
                     std::shared_ptr<Cache> block_cache = nullptr);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Returns an iterator over the table's entries. The table must outlive it.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) const;

  // Finds the first entry with key >= `key` (an internal key) in the one
  // data block that can contain `key`, and passes it to `on_entry` if there
  // is one. Reads at most one data block, and none if the filter rules out
  // the user key; `on_entry` is then not called.
  Status InternalGet(const ReadOptions& options, std::string_view key,
                     const EntryCallback& on_entry) const;

  uint64_t file_number() const { return file_number_; }
  uint64_t file_size() const { return file_size_; }
  bool has_filter() const { return filter_ != nullptr; }

 private:
  Table(std::unique_ptr<RandomAccessFile> file, const Comparator* comparator,
        uint64_t file_number, uint64_t file_size, std::unique_ptr<Block> index_block);

  // Reads and verifies the data block at `handle`, from `block_cache_` when
  // `use_cache` is set and the table has a cache (filling it on a miss).
  Status ReadDataBlock(const BlockHandle& handle, bool use_cache,
                       std::shared_ptr<const Block>* block) const;

  // Opens an iterator over the data block whose encoded handle is
  // `index_value`. The iterator keeps the block alive.
  std::unique_ptr<Iterator> NewDataBlockIterator(std::string_view index_value,
                                                 bool use_cache) const;

  void Record(std::atomic<uint64_t> Statistics::*counter, uint64_t n = 1) const {
    if (statistics_ != nullptr) {
      (statistics_.get()->*counter).fetch_add(n, std::memory_order_relaxed);
    }
  }

  // Prefixes `s` with the table's identity so errors say which file failed.
  Status Annotate(const Status& s) const;

  const std::unique_ptr<RandomAccessFile> file_;
  const Comparator* const comparator_;
  const uint64_t file_number_;
  const uint64_t file_size_;
  const std::unique_ptr<Block> index_block_;

  // Set once by Open before the table is shared, read-only afterwards.
  BlockContents filter_contents_;  // Owns the bytes filter_ reads.
  std::unique_ptr<FilterBlockReader> filter_;
  std::shared_ptr<Cache> block_cache_;
  std::shared_ptr<Statistics> statistics_;
};

}  // namespace kvlog
