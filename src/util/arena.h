#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace kvlog {

// A bump allocator for memtable entries and skiplist nodes. Allocations are
// never freed individually; everything goes away with the arena, which is
// exactly the lifetime of a memtable. Not thread-safe for allocation; the
// memtable's single writer is the only caller.
class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Returns uninitialized storage for `bytes` bytes (bytes > 0).
  char* Allocate(size_t bytes);

  // Like Allocate, but aligned for any fundamental type.
  char* AllocateAligned(size_t bytes);

  // Bytes reserved from the system, including unused space at the end of
  // each block. Safe to call from any thread while the writer allocates.
  size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  size_t alloc_bytes_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace kvlog
