#include "db/table_cache.h"

#include <cassert>

#include "db/filename.h"
#include "table/iterator_util.h"

namespace kvlog {

TableCache::TableCache(std::string dbname, const Options& options, const Comparator* comparator,
                       size_t capacity)
    : env_(options.env),
      dbname_(std::move(dbname)),
      options_(options),
      comparator_(comparator),
      capacity_(capacity),
      block_cache_(options.block_cache_capacity > 0
                       ? std::make_shared<Cache>(options.block_cache_capacity)
                       : nullptr) {
  assert(env_ != nullptr);
  assert(capacity_ > 0);
}

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             std::shared_ptr<Table>* table) {
  {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = tables_.find(file_number);
    if (it != tables_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second.lru_position);
      *table = it->second.table;
      return Status::OK();
    }
  }

  // Open outside the lock: it reads the footer and index from disk, and
  // other lookups should not wait for that. Two threads missing on the same
  // table may both open it; the second insert below just reuses the first.
  const std::string fname = TableFileName(dbname_, file_number);
  std::unique_ptr<RandomAccessFile> file;
  Status s = env_->NewRandomAccessFile(fname, &file);
  if (!s.ok()) {
    // A table listed in the MANIFEST must exist; a missing one is damage,
    // and must never surface as NotFound (which means "no such key").
    if (s.IsNotFound()) {
      return Status::Corruption("missing table file", fname);
    }
    return s;
  }
  std::shared_ptr<Table> opened;
  s = Table::Open(options_, comparator_, std::move(file), file_number, file_size, &opened,
                  block_cache_);
  if (!s.ok()) {
    return s;  // Not cached, so a later attempt retries the open.
  }

  std::lock_guard<std::mutex> guard(mu_);
  auto [it, inserted] = tables_.try_emplace(file_number);
  if (inserted) {
    lru_.push_front(file_number);
    it->second = Entry{std::move(opened), lru_.begin()};
    while (tables_.size() > capacity_) {
      const uint64_t victim = lru_.back();
      lru_.pop_back();
      tables_.erase(victim);
    }
  } else {
    lru_.splice(lru_.begin(), lru_, it->second.lru_position);
  }
  *table = tables_.at(file_number).table;
  return Status::OK();
}

std::unique_ptr<Iterator> TableCache::NewIterator(const ReadOptions& options, uint64_t file_number,
                                                  uint64_t file_size) {
  std::shared_ptr<Table> table;
  Status s = FindTable(file_number, file_size, &table);
  if (!s.ok()) {
    return NewErrorIterator(std::move(s));
  }
  std::unique_ptr<Iterator> iter = table->NewIterator(options);
  iter->RegisterCleanup([table = std::move(table)] {});
  return iter;
}

Status TableCache::Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                       std::string_view internal_key, const Table::EntryCallback& on_entry) {
  std::shared_ptr<Table> table;
  Status s = FindTable(file_number, file_size, &table);
  if (!s.ok()) {
    return s;
  }
  return table->InternalGet(options, internal_key, on_entry);
}

void TableCache::Evict(uint64_t file_number) {
  std::lock_guard<std::mutex> guard(mu_);
  auto it = tables_.find(file_number);
  if (it != tables_.end()) {
    lru_.erase(it->second.lru_position);
    tables_.erase(it);
  }
}

size_t TableCache::Size() const {
  std::lock_guard<std::mutex> guard(mu_);
  return tables_.size();
}

}  // namespace kvlog
