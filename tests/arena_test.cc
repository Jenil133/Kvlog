#include "util/arena.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace kvlog {
namespace {

TEST(ArenaTest, StartsEmpty) {
  Arena arena;
  EXPECT_EQ(0u, arena.MemoryUsage());
}

TEST(ArenaTest, AllocationsKeepTheirContents) {
  Arena arena;
  std::mt19937 rng(17);
  std::vector<std::pair<size_t, char*>> allocated;
  size_t total = 0;
  for (int i = 0; i < 20000; ++i) {
    size_t size;
    if (i % (20000 / 10) == 0) {
      size = static_cast<size_t>(i);
    } else {
      size = (rng() % 4000 == 0) ? rng() % 6000 : ((rng() % 10 == 0) ? rng() % 100 : rng() % 20);
    }
    if (size == 0) {
      size = 1;  // Zero-byte allocations are not allowed.
    }
    char* const r = (rng() % 10 == 0) ? arena.AllocateAligned(size) : arena.Allocate(size);
    for (size_t b = 0; b < size; ++b) {
      r[b] = static_cast<char>(i % 256);  // Fill with a pattern unique to this allocation.
    }
    total += size;
    allocated.emplace_back(size, r);
    EXPECT_GE(arena.MemoryUsage(), total);
  }
  for (size_t i = 0; i < allocated.size(); ++i) {
    const auto [size, p] = allocated[i];
    for (size_t b = 0; b < size; ++b) {
      ASSERT_EQ(static_cast<int>(i % 256), static_cast<int>(static_cast<unsigned char>(p[b])));
    }
  }
}

TEST(ArenaTest, AlignedAllocationsAreAligned) {
  Arena arena;
  for (int i = 1; i < 500; ++i) {
    arena.Allocate(static_cast<size_t>(i % 7) + 1);  // Misalign the bump pointer.
    char* const p = arena.AllocateAligned(static_cast<size_t>(i));
    EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(p) % alignof(std::max_align_t));
  }
}

}  // namespace
}  // namespace kvlog
