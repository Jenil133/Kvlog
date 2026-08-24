#pragma once

// CRC-32C (Castagnoli), the checksum on every log record and table block.
// CRC-32C rather than CRC-32 because it has better error detection for the
// record sizes involved and both ARMv8 and SSE4.2 compute it in hardware.

#include <cstddef>
#include <cstdint>

namespace kvlog::crc32c {

// Returns the CRC of data[0, n) appended to a string whose CRC is `crc`:
// Extend(Value(a), b) == Value(a + b).
uint32_t Extend(uint32_t crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

// Table-driven implementation, used when no hardware instruction is
// available. Exposed so tests can check the two implementations agree.
uint32_t ExtendPortable(uint32_t crc, const char* data, size_t n);

// True if Extend uses a CPU CRC instruction.
bool IsHardwareAccelerated();

// Stored checksums are masked. Computing the CRC of a string that embeds its
// own CRC (a log file containing table bytes, say) degrades the checksum's
// error detection; rotating and offsetting the stored value avoids that.
inline constexpr uint32_t kMaskDelta = 0xa282ead8u;

inline uint32_t Mask(uint32_t crc) { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }

inline uint32_t Unmask(uint32_t masked) {
  const uint32_t rotated = masked - kMaskDelta;
  return (rotated >> 17) | (rotated << 15);
}

}  // namespace kvlog::crc32c
