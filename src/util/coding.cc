#include "util/coding.h"

namespace kvlog {

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

char* EncodeVarint32(char* dst, uint32_t value) {
  return EncodeVarint64(dst, value);
}

char* EncodeVarint64(char* dst, uint64_t value) {
  auto* p = reinterpret_cast<unsigned char*>(dst);
  while (value >= 0x80) {
    *p++ = static_cast<unsigned char>(value | 0x80);
    value >>= 7;
  }
  *p++ = static_cast<unsigned char>(value);
  return reinterpret_cast<char*>(p);
}

void PutVarint32(std::string* dst, uint32_t value) {
  char buf[kMaxVarint32Length];
  char* end = EncodeVarint32(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutVarint64(std::string* dst, uint64_t value) {
  char buf[kMaxVarint64Length];
  char* end = EncodeVarint64(buf, value);
  dst->append(buf, static_cast<size_t>(end - buf));
}

void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value.data(), value.size());
}

int VarintLength(uint64_t value) {
  int len = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++len;
  }
  return len;
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    const uint32_t byte = static_cast<unsigned char>(*p++);
    // The fifth byte may only carry the top four bits of a 32-bit value.
    if (shift == 28 && byte > 0x0f) {
      return nullptr;
    }
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return p;
    }
  }
  return nullptr;
}

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    const uint64_t byte = static_cast<unsigned char>(*p++);
    // The tenth byte may only carry the top bit of a 64-bit value.
    if (shift == 63 && byte > 0x01) {
      return nullptr;
    }
    result |= (byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return p;
    }
  }
  return nullptr;
}

bool GetVarint32(std::string_view* input, uint32_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint32Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(q - p));
  return true;
}

bool GetVarint64(std::string_view* input, uint64_t* value) {
  const char* p = input->data();
  const char* limit = p + input->size();
  const char* q = GetVarint64Ptr(p, limit, value);
  if (q == nullptr) {
    return false;
  }
  input->remove_prefix(static_cast<size_t>(q - p));
  return true;
}

bool GetFixed32(std::string_view* input, uint32_t* value) {
  if (input->size() < 4) {
    return false;
  }
  *value = DecodeFixed32(input->data());
  input->remove_prefix(4);
  return true;
}

bool GetFixed64(std::string_view* input, uint64_t* value) {
  if (input->size() < 8) {
    return false;
  }
  *value = DecodeFixed64(input->data());
  input->remove_prefix(8);
  return true;
}

bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  uint32_t len = 0;
  if (!GetVarint32(input, &len) || input->size() < len) {
    return false;
  }
  *result = input->substr(0, len);
  input->remove_prefix(len);
  return true;
}

}  // namespace kvlog
