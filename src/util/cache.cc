#include "util/cache.h"

#include <cassert>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/hash.h"

namespace kvlog {

struct Cache::Handle {
  void* value = nullptr;
  Deleter deleter = nullptr;
  size_t charge = 0;
  uint64_t hash = 0;
  // References: one for the cache while `in_cache`, plus one per handle
  // given out. An entry sits in the LRU list exactly when it is in the cache
  // and nobody else holds it (refs == 1), which is when it may be evicted.
  uint32_t refs = 0;
  bool in_cache = false;
  Handle* prev = nullptr;
  Handle* next = nullptr;
  std::string key;
};

class Cache::Shard {
 public:
  Shard() { lru_.prev = lru_.next = &lru_; }
  Shard(const Shard&) = delete;
  Shard& operator=(const Shard&) = delete;

  ~Shard() {
    // Entries outside the LRU list are pinned: someone forgot a Release.
    assert(table_.size() == CountLru());
    for (Handle* e = lru_.next; e != &lru_;) {
      Handle* next = e->next;
      assert(e->in_cache && e->refs == 1);
      Free(e);
      e = next;
    }
  }

  void set_capacity(size_t capacity) { capacity_ = capacity; }

  Handle* Insert(std::string_view key, uint64_t hash, void* value, size_t charge,
                 Deleter deleter) {
    auto* e = new Handle;
    e->value = value;
    e->deleter = deleter;
    e->charge = charge;
    e->hash = hash;
    e->refs = 2;  // The cache and the returned handle.
    e->in_cache = true;
    e->key.assign(key.data(), key.size());

    std::vector<Handle*> garbage;
    {
      std::lock_guard<std::mutex> guard(mu_);
      auto [it, inserted] = table_.try_emplace(std::string_view(e->key), e);
      if (!inserted) {
        Handle* old = it->second;
        table_.erase(it);
        RemoveFromCache(old, &garbage);
        table_.emplace(std::string_view(e->key), e);
      }
      usage_ += charge;
      ++stats_.inserts;
      EvictToCapacity(&garbage);
    }
    FreeAll(garbage);
    return e;
  }

  Handle* Lookup(std::string_view key) {
    std::lock_guard<std::mutex> guard(mu_);
    auto it = table_.find(key);
    if (it == table_.end()) {
      ++stats_.misses;
      return nullptr;
    }
    ++stats_.hits;
    Handle* e = it->second;
    if (e->refs == 1) {
      ListRemove(e);  // Pinned now, so not evictable.
    }
    ++e->refs;
    return e;
  }

  void Release(Handle* e) {
    std::vector<Handle*> garbage;
    {
      std::lock_guard<std::mutex> guard(mu_);
      assert(e->refs > 0);
      --e->refs;
      if (e->refs == 0) {
        garbage.push_back(e);  // Already out of the cache.
      } else if (e->refs == 1 && e->in_cache) {
        ListAppend(e);  // Most recently used.
        // Entries inserted while others were pinned may have pushed the
        // shard over its capacity; settle that now that one is evictable.
        EvictToCapacity(&garbage);
      }
    }
    FreeAll(garbage);
  }

  void Erase(std::string_view key) {
    std::vector<Handle*> garbage;
    {
      std::lock_guard<std::mutex> guard(mu_);
      auto it = table_.find(key);
      if (it == table_.end()) {
        return;
      }
      Handle* e = it->second;
      table_.erase(it);
      RemoveFromCache(e, &garbage);
    }
    FreeAll(garbage);
  }

  size_t usage() const {
    std::lock_guard<std::mutex> guard(mu_);
    return usage_;
  }

  Stats stats() const {
    std::lock_guard<std::mutex> guard(mu_);
    return stats_;
  }

 private:
  static void Free(Handle* e) {
    e->deleter(e->key, e->value);
    delete e;
  }

  static void FreeAll(const std::vector<Handle*>& garbage) {
    // Deleters run without the shard lock held.
    for (Handle* e : garbage) {
      Free(e);
    }
  }

  // Takes `e`, already removed from table_, out of the cache. It is freed
  // (via `garbage`) unless a handle still pins it. Requires mu_.
  void RemoveFromCache(Handle* e, std::vector<Handle*>* garbage) {
    assert(e->in_cache);
    e->in_cache = false;
    usage_ -= e->charge;
    if (e->refs == 1) {
      ListRemove(e);
    }
    if (--e->refs == 0) {
      garbage->push_back(e);
    }
  }

  // Requires mu_.
  void EvictToCapacity(std::vector<Handle*>* garbage) {
    while (usage_ > capacity_ && lru_.next != &lru_) {
      Handle* oldest = lru_.next;
      table_.erase(std::string_view(oldest->key));
      RemoveFromCache(oldest, garbage);
      ++stats_.evictions;
    }
  }

  void ListRemove(Handle* e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
    e->prev = e->next = nullptr;
  }

  // Appends at the most recently used end (just before the dummy head).
  void ListAppend(Handle* e) {
    e->next = &lru_;
    e->prev = lru_.prev;
    e->prev->next = e;
    e->next->prev = e;
  }

  size_t CountLru() const {
    size_t n = 0;
    for (const Handle* e = lru_.next; e != &lru_; e = e->next) {
      ++n;
    }
    return n;
  }

  mutable std::mutex mu_;
  size_t capacity_ = 0;
  size_t usage_ = 0;
  // Dummy head of the circular LRU list; lru_.next is the least recently
  // used entry.
  Handle lru_;
  std::unordered_map<std::string_view, Handle*> table_;  // Keys point into the handles.
  Stats stats_;
};

int Cache::DefaultShardBits(size_t capacity) {
  int bits = 0;
  while (bits < kMaxDefaultShardBits && (capacity >> (bits + 1)) >= kMinDefaultShardCapacity) {
    ++bits;
  }
  return bits;
}

Cache::Cache(size_t capacity, int num_shard_bits)
    : capacity_(capacity), shard_bits_(num_shard_bits) {
  assert(num_shard_bits >= 0 && num_shard_bits <= 16);
  const size_t num_shards = size_t{1} << shard_bits_;
  shards_ = std::make_unique<Shard[]>(num_shards);
  const size_t per_shard = (capacity + num_shards - 1) / num_shards;
  for (size_t i = 0; i < num_shards; ++i) {
    shards_[i].set_capacity(per_shard);
  }
}

Cache::~Cache() = default;

Cache::Shard& Cache::ShardFor(uint64_t hash) const {
  // The top bits pick the shard; the shard's hash table uses its own hash.
  return shards_[shard_bits_ == 0 ? 0 : static_cast<size_t>(hash >> (64 - shard_bits_))];
}

Cache::Handle* Cache::Insert(std::string_view key, void* value, size_t charge, Deleter deleter) {
  const uint64_t hash = Hash64(key);
  return ShardFor(hash).Insert(key, hash, value, charge, deleter);
}

Cache::Handle* Cache::Lookup(std::string_view key) { return ShardFor(Hash64(key)).Lookup(key); }

void Cache::Release(Handle* handle) { ShardFor(handle->hash).Release(handle); }

void* Cache::Value(Handle* handle) const { return handle->value; }

void Cache::Erase(std::string_view key) { ShardFor(Hash64(key)).Erase(key); }

size_t Cache::TotalCharge() const {
  size_t total = 0;
  for (size_t i = 0; i < (size_t{1} << shard_bits_); ++i) {
    total += shards_[i].usage();
  }
  return total;
}

Cache::Stats Cache::GetStats() const {
  Stats total;
  for (size_t i = 0; i < (size_t{1} << shard_bits_); ++i) {
    const Stats s = shards_[i].stats();
    total.hits += s.hits;
    total.misses += s.misses;
    total.inserts += s.inserts;
    total.evictions += s.evictions;
  }
  return total;
}

}  // namespace kvlog
