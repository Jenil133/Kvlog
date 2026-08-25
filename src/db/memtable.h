#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "kvlog/iterator.h"
#include "kvlog/status.h"
#include "util/arena.h"

namespace kvlog {

// The in-memory write buffer: a skiplist of internal keys in an arena.
//
// Each entry is a single arena allocation:
//
//   varint32(internal_key.size()) | user_key | tag | varint32(value.size()) | value
//
// Deletions are stored as entries of type kTypeDeletion (tombstones) so that
// they shadow older values in tables until compaction can drop both.
//
// Threading follows the skiplist: one writer at a time (the DB's committing
// writer), any number of concurrent readers. A memtable is shared through
// std::shared_ptr: readers and iterators hold a reference while they use it,
// and it is freed when the last one lets go after it has been flushed.
class MemTable {
 public:
  explicit MemTable(const InternalKeyComparator& comparator);
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Bytes allocated for entries and index nodes. Safe to call concurrently
  // with Add().
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  // Bytes allocated since the memtable was created. An empty memtable
  // already owns an arena block (the skiplist head lives in it), so this,
  // not ApproximateMemoryUsage(), is what is compared with the write buffer
  // size: otherwise a buffer no larger than one block would count as full
  // before its first entry.
  size_t BytesAdded() const { return arena_.MemoryUsage() - empty_usage_; }

  bool Empty() const { return num_entries_.load(std::memory_order_acquire) == 0; }
  uint64_t NumEntries() const { return num_entries_.load(std::memory_order_acquire); }

  // Adds an entry. Requires: external synchronization with other Add()
  // calls, and (sequence, user key) not already present.
  void Add(SequenceNumber sequence, ValueType type, std::string_view user_key,
           std::string_view value);

  // Looks up the newest entry for key.user_key() with a sequence number no
  // larger than the lookup sequence. Returns true if one exists: for a value,
  // stores it in *value and sets *s to OK; for a tombstone, sets *s to
  // NotFound. Returns false if this memtable has no visible entry for the key.
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

  // Returns an iterator over the internal keys (key()) and values in
  // internal key order. The memtable must outlive the iterator.
  std::unique_ptr<Iterator> NewIterator() const;

 private:
  friend class MemTableIterator;

  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    // Compares two entries by their internal keys.
    int operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

  KeyComparator comparator_;
  Arena arena_;
  Table table_;
  size_t empty_usage_ = 0;  // arena_.MemoryUsage() right after construction.
  std::atomic<uint64_t> num_entries_{0};
};

}  // namespace kvlog
