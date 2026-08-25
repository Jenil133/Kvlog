#include "db/dbformat.h"

#include <cstdio>
#include <cstring>

namespace kvlog {

void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key.data(), key.user_key.size());
  PutFixed64(result, PackSequenceAndType(key.sequence, key.type));
}

bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result) {
  if (internal_key.size() < kTagSize) {
    return false;
  }
  const uint64_t tag = ExtractTag(internal_key);
  const uint8_t type = static_cast<uint8_t>(tag & 0xff);
  if (type != kTypeValue && type != kTypeDeletion) {
    return false;
  }
  result->user_key = ExtractUserKey(internal_key);
  result->sequence = tag >> 8;
  result->type = static_cast<ValueType>(type);
  return true;
}

int InternalKeyComparator::Compare(std::string_view a, std::string_view b) const {
  int r = user_comparator_->Compare(ExtractUserKey(a), ExtractUserKey(b));
  if (r == 0) {
    // Larger tags (newer sequence numbers) sort first.
    const uint64_t a_tag = ExtractTag(a);
    const uint64_t b_tag = ExtractTag(b);
    if (a_tag > b_tag) {
      r = -1;
    } else if (a_tag < b_tag) {
      r = +1;
    }
  }
  return r;
}

std::string InternalKey::DebugString() const {
  ParsedInternalKey parsed;
  if (!ParseInternalKey(rep_, &parsed)) {
    return "(bad internal key)";
  }
  std::string result;
  for (const char c : parsed.user_key) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= ' ' && byte <= '~' && byte != '\\') {
      result.push_back(c);
    } else {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\x%02x", byte);
      result.append(escaped);
    }
  }
  result.append(" @ ");
  result.append(std::to_string(parsed.sequence));
  result.append(parsed.type == kTypeValue ? " : value" : " : deletion");
  return result;
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  const size_t needed = user_key.size() + kTagSize + kMaxVarint32Length;
  char* dst = inline_;
  if (needed > sizeof(inline_)) {
    heap_ = std::make_unique_for_overwrite<char[]>(needed);
    dst = heap_.get();
  }
  start_ = dst;
  dst = EncodeVarint32(dst, static_cast<uint32_t>(user_key.size() + kTagSize));
  kstart_ = dst;
  if (!user_key.empty()) {
    std::memcpy(dst, user_key.data(), user_key.size());
  }
  dst += user_key.size();
  EncodeFixed64(dst, PackSequenceAndType(sequence, kValueTypeForSeek));
  dst += kTagSize;
  end_ = dst;
}

}  // namespace kvlog
