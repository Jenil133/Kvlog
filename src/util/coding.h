#pragma once

// Integer encodings shared by every on-disk format. Fixed-width integers are
// little-endian. Varints are LEB128: seven bits per byte, low bits first, with
// the high bit set on every byte except the last.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kvlog {

inline constexpr int kMaxVarint32Length = 5;
inline constexpr int kMaxVarint64Length = 10;

inline void EncodeFixed32(char* dst, uint32_t value) {
  auto* p = reinterpret_cast<unsigned char*>(dst);
  p[0] = static_cast<unsigned char>(value);
  p[1] = static_cast<unsigned char>(value >> 8);
  p[2] = static_cast<unsigned char>(value >> 16);
  p[3] = static_cast<unsigned char>(value >> 24);
}

inline void EncodeFixed64(char* dst, uint64_t value) {
  auto* p = reinterpret_cast<unsigned char*>(dst);
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<unsigned char>(value >> (8 * i));
  }
}

inline uint32_t DecodeFixed32(const char* src) {
  const auto* p = reinterpret_cast<const unsigned char*>(src);
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* src) {
  const auto* p = reinterpret_cast<const unsigned char*>(src);
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return result;
}

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
// Appends varint32(value.size()) followed by the bytes of value.
void PutLengthPrefixed(std::string* dst, std::string_view value);

// Writes a varint to dst, which needs room for kMaxVarint*Length bytes, and
// returns a pointer just past the last byte written.
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

// Number of bytes the varint encoding of `value` occupies.
int VarintLength(uint64_t value);

// Decodes a varint from [p, limit). Returns a pointer just past it, or
// nullptr if the input is truncated or encodes a value that does not fit.
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

// Consume one encoded value from the front of *input. On malformed input
// they return false and leave *input in an unspecified state.
bool GetVarint32(std::string_view* input, uint32_t* value);
bool GetVarint64(std::string_view* input, uint64_t* value);
bool GetFixed32(std::string_view* input, uint32_t* value);
bool GetFixed64(std::string_view* input, uint64_t* value);
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);

}  // namespace kvlog
