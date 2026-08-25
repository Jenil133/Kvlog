#pragma once

// Bloom filters over byte-string keys.
//
// Encoded filter:  bit array (ceil(bits / 8) bytes) | probe count k (uint8)
//
// Bit i lives in byte i / 8 at bit position i % 8. Each key is hashed once
// with Hash64 (XXH64, seed 0); the low and high 32-bit halves seed double
// hashing (Kirsch and Mitzenmacher): probe j sets bit (h1 + j * h2) mod bits,
// for j in [0, k). Two independent hashes simulate k of them with no
// measurable loss in false-positive rate.
//
// With b bits per key the false-positive rate is minimised at
// k = b * ln 2; 10 bits per key with k = 7 gives about 0.8%.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kvlog {

// Probe count used for `bits_per_key`: round(bits_per_key * ln 2), clamped
// to [1, 30].
int BloomProbesForBitsPerKey(int bits_per_key);

// The value fed to the filter for `key`. Builders can record it per key and
// build the filter later, keeping 8 bytes per key instead of the key.
uint64_t BloomHash(std::string_view key);

// Appends a filter covering `key_hashes` (BloomHash values) to `dst`, using
// `bits_per_key` bits per key (at least 64 bits in total). `bits_per_key`
// must be positive.
void BuildBloomFilter(const std::vector<uint64_t>& key_hashes, int bits_per_key,
                      std::string* dst);

// Read-only view of an encoded filter. The bytes must outlive the reader.
class BloomFilterReader {
 public:
  BloomFilterReader() = default;  // Matches every key.
  explicit BloomFilterReader(std::string_view filter);

  // False only if `key` was certainly not added. A filter that is empty, too
  // short, or uses a probe count this code does not know (a future format)
  // matches everything: a filter may only ever save work, never hide a key.
  bool KeyMayMatch(std::string_view key) const { return HashMayMatch(BloomHash(key)); }
  bool HashMayMatch(uint64_t key_hash) const;

 private:
  const unsigned char* bits_ = nullptr;
  uint64_t num_bits_ = 0;
  int num_probes_ = 0;
};

}  // namespace kvlog
