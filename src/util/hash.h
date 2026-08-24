#pragma once

// A stable 64-bit hash: XXH64 (xxHash, 64-bit variant, by Yann Collet).
//
// Bloom filters are stored in table files, so the hash that places keys in
// them is part of the on-disk format: it must give the same result on every
// platform, compiler and run. std::hash promises none of that, hence this
// in-tree implementation. Inputs are read as little-endian regardless of the
// host byte order, so the output matches the reference XXH64 everywhere.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kvlog {

uint64_t Hash64(const char* data, size_t n, uint64_t seed);

inline uint64_t Hash64(std::string_view data, uint64_t seed = 0) {
  return Hash64(data.data(), data.size(), seed);
}

}  // namespace kvlog
