#pragma once

// Key generation, key-choice distributions and latency percentiles for
// kvlog_bench. Header-only so the unit tests can check them directly.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace kvlog::bench {

// Fixed-width decimal keys, so byte order equals numeric order.
inline std::string BenchKey(uint64_t index) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llu", static_cast<unsigned long long>(index));
  return buf;
}

// A key that is never written but sorts between BenchKey(index) and
// BenchKey(index + 1). Lookups for it land inside the key range of the
// table files, so only a Bloom filter (not the range check) can skip them.
inline std::string MissingKey(uint64_t index) { return BenchKey(index) + "."; }

// Visits every index of [begin, begin + count) exactly once. Scattered:
// i -> begin + (i * kStride + offset) mod count, where kStride is a prime
// larger than any count used here, hence coprime with count, which makes the
// map a bijection. Sequential: i -> begin + i.
class ScatteredRange {
 public:
  static constexpr uint64_t kStride = 2654435761ULL;  // Prime, about 2^31.3.

  static ScatteredRange Sequential(uint64_t begin, uint64_t count) {
    return ScatteredRange(begin, count, 1, 0);
  }
  static ScatteredRange Scattered(uint64_t begin, uint64_t count, uint64_t seed) {
    return ScatteredRange(begin, count, kStride, count == 0 ? 0 : seed % count);
  }

  // Requires: i < count <= 2^32, which keeps i * kStride + offset below
  // 2^64 and count below kStride.
  uint64_t At(uint64_t i) const { return begin_ + (i * stride_ + offset_) % count_; }

  uint64_t count() const { return count_; }

 private:
  ScatteredRange(uint64_t begin, uint64_t count, uint64_t stride, uint64_t offset)
      : begin_(begin), count_(count), stride_(stride), offset_(offset) {}

  uint64_t begin_;
  uint64_t count_;
  uint64_t stride_;
  uint64_t offset_;
};

// Zipfian ranks over [0, n) as in the YCSB generator (Gray et al., "Quickly
// generating billion-record synthetic databases"): rank 0 is the most
// popular. The normalising constant zeta(n) is extended incrementally when
// the item count grows, so a growing dataset does not pay O(n) per step.
class ZipfianGenerator {
 public:
  explicit ZipfianGenerator(uint64_t n, double theta = 0.99) : theta_(theta) {
    zeta2_ = 1.0 + std::pow(0.5, theta_);
    Grow(n);
  }

  uint64_t items() const { return n_; }

  // Extends the item count to `n` (never shrinks).
  void Grow(uint64_t n) {
    for (uint64_t i = n_ + 1; i <= n; ++i) {
      zetan_ += 1.0 / std::pow(static_cast<double>(i), theta_);
    }
    n_ = std::max(n_, n);
    alpha_ = 1.0 / (1.0 - theta_);
    eta_ = (1.0 - std::pow(2.0 / static_cast<double>(n_), 1.0 - theta_)) / (1.0 - zeta2_ / zetan_);
  }

  template <typename Rng>
  uint64_t Next(Rng* rng) {
    const double u = std::uniform_real_distribution<double>(0.0, 1.0)(*rng);
    const double uz = u * zetan_;
    if (uz < 1.0 || n_ < 2) {
      return 0;
    }
    if (uz < zeta2_) {
      return 1;
    }
    const auto rank = static_cast<uint64_t>(static_cast<double>(n_) *
                                            std::pow(eta_ * u - eta_ + 1.0, alpha_));
    return std::min(rank, n_ - 1);
  }

 private:
  double theta_;
  double zeta2_ = 0;
  double zetan_ = 0;
  double alpha_ = 0;
  double eta_ = 0;
  uint64_t n_ = 0;
};

// Picks key indices in [0, n) uniformly or with a zipfian skew. Zipfian
// ranks are scattered over the key space with a 64-bit mix, so the hot keys
// are spread across all table files instead of sitting in the first one.
class KeyChooser {
 public:
  KeyChooser(bool zipfian, uint64_t n) : zipfian_(zipfian), n_(n), zipf_(zipfian ? n : 1) {}

  void Grow(uint64_t n) {
    n_ = std::max(n_, n);
    if (zipfian_) {
      zipf_.Grow(n_);
    }
  }

  template <typename Rng>
  uint64_t Next(Rng* rng) {
    if (!zipfian_) {
      return std::uniform_int_distribution<uint64_t>(0, n_ - 1)(*rng);
    }
    return Mix(zipf_.Next(rng)) % n_;
  }

  // SplitMix64 finaliser: a fixed bijection on 64-bit values.
  static uint64_t Mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }

 private:
  bool zipfian_;
  uint64_t n_;
  ZipfianGenerator zipf_;
};

// Per-operation latencies of one measured phase.
class LatencyRecorder {
 public:
  void Add(double micros) { samples_.push_back(static_cast<float>(micros)); }
  void Clear() { samples_.clear(); }
  size_t count() const { return samples_.size(); }

  // Nearest-rank percentile, p in [0, 100]. 0 when empty.
  double Percentile(double p) {
    if (samples_.empty()) {
      return 0;
    }
    const size_t rank = static_cast<size_t>(
        std::ceil(p / 100.0 * static_cast<double>(samples_.size())));
    const size_t index = std::clamp<size_t>(rank, 1, samples_.size()) - 1;
    std::nth_element(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(index),
                     samples_.end());
    return samples_[index];
  }

 private:
  std::vector<float> samples_;
};

}  // namespace kvlog::bench
