#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "kvlog/env.h"
#include "kvlog/iterator.h"
#include "kvlog/options.h"
#include "kvlog/status.h"
#include "table/table.h"
#include "util/cache.h"
#include "util/comparator.h"

namespace kvlog {

// Keeps recently used table files open. Opening a table costs a file open
// plus reading its footer and index block, so lookups reuse open tables.
//
// The cache holds at most `capacity` tables, evicting the least recently
// used. Tables are shared through std::shared_ptr: an evicted table that an
// iterator or lookup is still reading stays open until they finish.
//
// The cache also owns the block cache shared by every table it opens: an LRU
// of data blocks bounded by options.block_cache_capacity bytes (none if 0),
// keyed by (file number, block offset). Blocks are keyed by file number
// rather than by table object, so a table closed and reopened here finds
// its blocks still cached. Blocks of deleted tables are not looked up again
// and age out of the LRU.
// Thread-safe.
class TableCache {
 public:
  // `options` and `comparator` must outlive the cache.
  TableCache(std::string dbname, const Options& options, const Comparator* comparator,
             size_t capacity);
  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Returns an iterator over table `file_number` (of `file_size` bytes). The
  // iterator keeps the table open for its whole lifetime.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options, uint64_t file_number,
                                        uint64_t file_size);

  // Table::InternalGet on table `file_number`.
  Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
             std::string_view internal_key, const Table::EntryCallback& on_entry);

  // Drops the cached table for a file that is being deleted.
  void Evict(uint64_t file_number);

  // Number of tables currently cached.
  size_t Size() const;

  // The shared block cache; nullptr when block_cache_capacity is 0.
  Cache* block_cache() const { return block_cache_.get(); }

 private:
  struct Entry {
    std::shared_ptr<Table> table;
    std::list<uint64_t>::iterator lru_position;
  };

  Status FindTable(uint64_t file_number, uint64_t file_size, std::shared_ptr<Table>* table);

  Env* const env_;
  const std::string dbname_;
  const Options& options_;
  const Comparator* const comparator_;
  const size_t capacity_;
  const std::shared_ptr<Cache> block_cache_;

  mutable std::mutex mu_;
  std::list<uint64_t> lru_;  // Most recently used first.
  std::unordered_map<uint64_t, Entry> tables_;
};

}  // namespace kvlog
