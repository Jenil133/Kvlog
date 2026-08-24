#include "util/hash.h"

#include "util/coding.h"

namespace kvlog {

namespace {

constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t kPrime3 = 0x165667B19E3779F9ull;
constexpr uint64_t kPrime4 = 0x85EBCA77C2B2AE63ull;
constexpr uint64_t kPrime5 = 0x27D4EB2F165667C5ull;

constexpr uint64_t RotateLeft(uint64_t x, int bits) { return (x << bits) | (x >> (64 - bits)); }

constexpr uint64_t Round(uint64_t acc, uint64_t input) {
  acc += input * kPrime2;
  acc = RotateLeft(acc, 31);
  return acc * kPrime1;
}

constexpr uint64_t MergeRound(uint64_t acc, uint64_t value) {
  acc ^= Round(0, value);
  return acc * kPrime1 + kPrime4;
}

}  // namespace

uint64_t Hash64(const char* data, size_t n, uint64_t seed) {
  const char* p = data;
  const char* const end = data + n;
  uint64_t h;

  if (n >= 32) {
    // Four independent lanes consume 32-byte stripes.
    uint64_t v1 = seed + kPrime1 + kPrime2;
    uint64_t v2 = seed + kPrime2;
    uint64_t v3 = seed;
    uint64_t v4 = seed - kPrime1;
    const char* const limit = end - 32;
    do {
      v1 = Round(v1, DecodeFixed64(p));
      v2 = Round(v2, DecodeFixed64(p + 8));
      v3 = Round(v3, DecodeFixed64(p + 16));
      v4 = Round(v4, DecodeFixed64(p + 24));
      p += 32;
    } while (p <= limit);
    h = RotateLeft(v1, 1) + RotateLeft(v2, 7) + RotateLeft(v3, 12) + RotateLeft(v4, 18);
    h = MergeRound(h, v1);
    h = MergeRound(h, v2);
    h = MergeRound(h, v3);
    h = MergeRound(h, v4);
  } else {
    h = seed + kPrime5;
  }

  h += static_cast<uint64_t>(n);

  // The tail: 8 bytes, then 4, then single bytes.
  while (end - p >= 8) {
    h ^= Round(0, DecodeFixed64(p));
    h = RotateLeft(h, 27) * kPrime1 + kPrime4;
    p += 8;
  }
  if (end - p >= 4) {
    h ^= static_cast<uint64_t>(DecodeFixed32(p)) * kPrime1;
    h = RotateLeft(h, 23) * kPrime2 + kPrime3;
    p += 4;
  }
  while (p < end) {
    h ^= static_cast<uint64_t>(static_cast<unsigned char>(*p)) * kPrime5;
    h = RotateLeft(h, 11) * kPrime1;
    ++p;
  }

  // Final avalanche.
  h ^= h >> 33;
  h *= kPrime2;
  h ^= h >> 29;
  h *= kPrime3;
  h ^= h >> 32;
  return h;
}

}  // namespace kvlog
