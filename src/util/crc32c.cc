#include "util/crc32c.h"

#include <array>
#include <cstring>

#include "util/coding.h"

#if defined(__ARM_FEATURE_CRC32) && defined(__BYTE_ORDER__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_acle.h>
#define KVLOG_CRC32C_ARM 1
#elif defined(__SSE4_2__) && defined(__x86_64__)
#include <nmmintrin.h>
#define KVLOG_CRC32C_SSE42 1
#endif

namespace kvlog::crc32c {
namespace {

// Bit-reflected form of the Castagnoli polynomial 0x1EDC6F41.
constexpr uint32_t kPolynomial = 0x82f63b78u;

using Tables = std::array<std::array<uint32_t, 256>, 8>;

// Tables for slicing-by-8: tables[k][b] is the CRC contribution of byte b
// followed by k zero bytes, which lets the loop fold 8 input bytes per step.
constexpr Tables MakeTables() {
  Tables tables{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (kPolynomial & (0u - (crc & 1u)));
    }
    tables[0][i] = crc;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (size_t k = 1; k < 8; ++k) {
      const uint32_t prev = tables[k - 1][i];
      tables[k][i] = (prev >> 8) ^ tables[0][prev & 0xffu];
    }
  }
  return tables;
}

constexpr Tables kTables = MakeTables();

#if defined(KVLOG_CRC32C_ARM)
uint32_t ExtendHardware(uint32_t crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint32_t state = ~crc;
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, p, sizeof(word));
    state = __crc32cd(state, word);
    p += 8;
    n -= 8;
  }
  if (n >= 4) {
    uint32_t word;
    std::memcpy(&word, p, sizeof(word));
    state = __crc32cw(state, word);
    p += 4;
    n -= 4;
  }
  while (n > 0) {
    state = __crc32cb(state, *p++);
    --n;
  }
  return ~state;
}
#elif defined(KVLOG_CRC32C_SSE42)
uint32_t ExtendHardware(uint32_t crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint64_t state = static_cast<uint32_t>(~crc);
  while (n >= 8) {
    uint64_t word;
    std::memcpy(&word, p, sizeof(word));
    state = _mm_crc32_u64(state, word);
    p += 8;
    n -= 8;
  }
  uint32_t state32 = static_cast<uint32_t>(state);
  while (n > 0) {
    state32 = _mm_crc32_u8(state32, *p++);
    --n;
  }
  return ~state32;
}
#endif

}  // namespace

uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n) {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint32_t state = ~crc;
  while (n >= 8) {
    const uint32_t lo = DecodeFixed32(reinterpret_cast<const char*>(p)) ^ state;
    const uint32_t hi = DecodeFixed32(reinterpret_cast<const char*>(p + 4));
    state = kTables[7][lo & 0xffu] ^ kTables[6][(lo >> 8) & 0xffu] ^
            kTables[5][(lo >> 16) & 0xffu] ^ kTables[4][lo >> 24] ^ kTables[3][hi & 0xffu] ^
            kTables[2][(hi >> 8) & 0xffu] ^ kTables[1][(hi >> 16) & 0xffu] ^
            kTables[0][hi >> 24];
    p += 8;
    n -= 8;
  }
  while (n > 0) {
    state = kTables[0][(state ^ *p++) & 0xffu] ^ (state >> 8);
    --n;
  }
  return ~state;
}

uint32_t Extend(uint32_t crc, const char* data, size_t n) {
#if defined(KVLOG_CRC32C_ARM) || defined(KVLOG_CRC32C_SSE42)
  return ExtendHardware(crc, data, n);
#else
  return ExtendPortable(crc, data, n);
#endif
}

bool IsHardwareAccelerated() {
#if defined(KVLOG_CRC32C_ARM) || defined(KVLOG_CRC32C_SSE42)
  return true;
#else
  return false;
#endif
}

}  // namespace kvlog::crc32c
