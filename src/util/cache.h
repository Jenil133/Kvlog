#pragma once

// A sharded LRU cache with a byte capacity and reference-counted handles.
//
// Each entry has a key, an opaque value, a charge (its cost against the
// capacity, normally its size in bytes) and a deleter that frees the value.
// Lookup and Insert return a Handle that pins the entry: a pinned entry is
// never freed, even if it is evicted or erased meanwhile (it just stops
// being findable). The value is freed once it is out of the cache and its
// last handle is released.
//
// Only unpinned entries can be evicted, so the total charge can exceed the
// capacity while many handles are held; it drops back within the capacity
// as they are released.
//
// Keys are spread over 2^num_shard_bits shards by hash, each with its own
// mutex and an equal share of the capacity, so concurrent readers rarely
// contend. Thread-safe.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace kvlog {

class Cache {
 public:
  struct Handle;  // Opaque.
  using Deleter = void (*)(std::string_view key, void* value);

  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t inserts = 0;
    // Entries dropped to make room (not counting Erase or replacement).
    uint64_t evictions = 0;
  };

  static constexpr int kMaxDefaultShardBits = 4;  // 16 shards.
  static constexpr size_t kMinDefaultShardCapacity = 256 * 1024;

  // Shard count for `capacity` when none is given: up to 16 shards, but
  // fewer for small caches so every shard can still hold a few large
  // entries (a shard smaller than one entry could never keep it).
  static int DefaultShardBits(size_t capacity);

  explicit Cache(size_t capacity) : Cache(capacity, DefaultShardBits(capacity)) {}
  Cache(size_t capacity, int num_shard_bits);
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;
  // Requires: every handle has been released.
  ~Cache();

  // Inserts `value` under `key`, replacing (and unpinning from the cache)
  // any entry with the same key, and returns a handle to the new entry. The
  // cache owns `value` from now on and frees it with `deleter`. Least
  // recently used unpinned entries are evicted until the charge fits.
  Handle* Insert(std::string_view key, void* value, size_t charge, Deleter deleter);

  // Returns a handle to the entry for `key` and marks it most recently used,
  // or nullptr if there is none.
  Handle* Lookup(std::string_view key);

  // Releases a handle from Insert or Lookup. Each handle is released once.
  void Release(Handle* handle);

  // The value of a handle that has not been released yet.
  void* Value(Handle* handle) const;

  // Removes the entry for `key`, if any. Its value is freed once it is no
  // longer pinned.
  void Erase(std::string_view key);

  // A new number on every call, for clients that share one cache and need
  // to keep their keys apart.
  uint64_t NewId() { return last_id_.fetch_add(1, std::memory_order_relaxed) + 1; }

  size_t capacity() const { return capacity_; }
  // Sum of the charges of the entries in the cache. An entry stops counting
  // as soon as it is evicted, erased or replaced, even while a handle still
  // pins it, so memory held by outstanding handles can exceed this.
  size_t TotalCharge() const;
  Stats GetStats() const;

 private:
  class Shard;

  Shard& ShardFor(uint64_t hash) const;

  const size_t capacity_;
  const int shard_bits_;
  std::unique_ptr<Shard[]> shards_;
  std::atomic<uint64_t> last_id_{0};
};

}  // namespace kvlog
