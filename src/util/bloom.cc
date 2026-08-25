#include "util/bloom.h"

#include <algorithm>
#include <cmath>

#include "util/hash.h"

namespace kvlog {

namespace {

constexpr int kMaxProbes = 30;
constexpr uint64_t kMinFilterBits = 64;

}  // namespace

int BloomProbesForBitsPerKey(int bits_per_key) {
  const long probes = std::lround(bits_per_key * 0.69314718055994531);  // ln 2
  return static_cast<int>(std::clamp(probes, 1L, static_cast<long>(kMaxProbes)));
}

uint64_t BloomHash(std::string_view key) { return Hash64(key, /*seed=*/0); }

void BuildBloomFilter(const std::vector<uint64_t>& key_hashes, int bits_per_key,
                      std::string* dst) {
  const int num_probes = BloomProbesForBitsPerKey(bits_per_key);
  // A tiny bit array has a high false-positive rate whatever the key count,
  // so small filters get a floor.
  uint64_t bits = static_cast<uint64_t>(key_hashes.size()) * static_cast<uint64_t>(bits_per_key);
  bits = std::max(bits, kMinFilterBits);
  const uint64_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  const size_t start = dst->size();
  dst->resize(start + static_cast<size_t>(bytes), '\0');
  dst->push_back(static_cast<char>(num_probes));
  auto* array = reinterpret_cast<unsigned char*>(dst->data() + start);
  for (const uint64_t h : key_hashes) {
    const uint32_t h1 = static_cast<uint32_t>(h);
    const uint32_t h2 = static_cast<uint32_t>(h >> 32);
    uint64_t position = h1;
    for (int j = 0; j < num_probes; ++j) {
      const uint64_t bit = position % bits;
      array[bit / 8] |= static_cast<unsigned char>(1u << (bit % 8));
      position += h2;
    }
  }
}

BloomFilterReader::BloomFilterReader(std::string_view filter) {
  if (filter.size() < 2) {
    return;
  }
  const int probes = static_cast<unsigned char>(filter.back());
  if (probes < 1 || probes > kMaxProbes) {
    return;
  }
  bits_ = reinterpret_cast<const unsigned char*>(filter.data());
  num_bits_ = static_cast<uint64_t>(filter.size() - 1) * 8;
  num_probes_ = probes;
}

bool BloomFilterReader::HashMayMatch(uint64_t key_hash) const {
  if (num_probes_ == 0) {
    return true;
  }
  const uint32_t h1 = static_cast<uint32_t>(key_hash);
  const uint32_t h2 = static_cast<uint32_t>(key_hash >> 32);
  uint64_t position = h1;
  for (int j = 0; j < num_probes_; ++j) {
    const uint64_t bit = position % num_bits_;
    if ((bits_[bit / 8] & (1u << (bit % 8))) == 0) {
      return false;
    }
    position += h2;
  }
  return true;
}

}  // namespace kvlog
