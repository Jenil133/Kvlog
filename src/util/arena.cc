#include "util/arena.h"

#include <cassert>
#include <cstdint>

namespace kvlog {

char* Arena::AllocateFallback(size_t bytes) {
  if (bytes > kBlockSize / 4) {
    // Large objects get a block of their own so the remainder of the current
    // block is not wasted.
    return AllocateNewBlock(bytes);
  }
  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;
  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateAligned(size_t bytes) {
  constexpr size_t kAlign = alignof(std::max_align_t);
  static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of two");
  const size_t misalignment = reinterpret_cast<uintptr_t>(alloc_ptr_) & (kAlign - 1);
  const size_t padding = misalignment == 0 ? 0 : kAlign - misalignment;
  const size_t needed = bytes + padding;
  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + padding;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    // New blocks come from operator new[], which aligns for max_align_t.
    result = AllocateFallback(bytes);
  }
  assert((reinterpret_cast<uintptr_t>(result) & (kAlign - 1)) == 0);
  return result;
}

char* Arena::AllocateNewBlock(size_t block_bytes) {
  blocks_.push_back(std::make_unique_for_overwrite<char[]>(block_bytes));
  memory_usage_.fetch_add(block_bytes + sizeof(char*), std::memory_order_relaxed);
  return blocks_.back().get();
}

}  // namespace kvlog
