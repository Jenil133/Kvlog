#pragma once

#include <cstdint>

namespace kvlog {

// A small, fast, deterministic pseudo-random generator (Park-Miller minimal
// standard). Used for skiplist node heights, where reproducibility matters
// more than statistical quality.
class Random {
 public:
  explicit Random(uint32_t seed) : seed_(seed & 0x7fffffffu) {
    // 0 and 2^31 - 1 are fixed points of the recurrence.
    if (seed_ == 0 || seed_ == 2147483647u) {
      seed_ = 1;
    }
  }

  uint32_t Next() {
    constexpr uint32_t kModulus = 2147483647u;  // 2^31 - 1
    constexpr uint64_t kMultiplier = 16807;     // 7^5
    // seed_ = (seed_ * A) % M, using 2^31 == 1 (mod M) to avoid a division.
    const uint64_t product = seed_ * kMultiplier;
    seed_ = static_cast<uint32_t>((product >> 31) + (product & kModulus));
    if (seed_ > kModulus) {
      seed_ -= kModulus;
    }
    return seed_;
  }

  // Uniform in [0, n). Requires n > 0.
  uint32_t Uniform(uint32_t n) { return Next() % n; }

  // True with probability 1/n. Requires n > 0.
  bool OneIn(uint32_t n) { return Next() % n == 0; }

 private:
  uint32_t seed_;
};

}  // namespace kvlog
