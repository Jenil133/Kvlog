#include "db/memtable.h"

#include <cstring>

#include "util/coding.h"

namespace kvlog {
namespace {

// Decodes the length-prefixed string at `data`. Entries are produced by
// MemTable::Add itself, so the encoding is trusted.
std::string_view GetLengthPrefixedSlice(const char* data) {
  uint32_t length = 0;
  const char* p = GetVarint32Ptr(data, data + kMaxVarint32Length, &length);
  assert(p != nullptr);
  return std::string_view(p, length);
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return comparator.Compare(GetLengthPrefixedSlice(a), GetLengthPrefixedSlice(b));
}

MemTable::MemTable(const InternalKeyComparator& comparator)
    : comparator_(comparator), table_(comparator_, &arena_), empty_usage_(arena_.MemoryUsage()) {}

void MemTable::Add(SequenceNumber sequence, ValueType type, std::string_view user_key,
                   std::string_view value) {
  const size_t internal_key_size = user_key.size() + kTagSize;
  const size_t encoded_size = static_cast<size_t>(VarintLength(internal_key_size)) +
                              internal_key_size +
                              static_cast<size_t>(VarintLength(value.size())) + value.size();
  char* const buf = arena_.Allocate(encoded_size);
  char* p = EncodeVarint32(buf, static_cast<uint32_t>(internal_key_size));
  if (!user_key.empty()) {
    std::memcpy(p, user_key.data(), user_key.size());
  }
  p += user_key.size();
  EncodeFixed64(p, PackSequenceAndType(sequence, type));
  p += kTagSize;
  p = EncodeVarint32(p, static_cast<uint32_t>(value.size()));
  if (!value.empty()) {
    std::memcpy(p, value.data(), value.size());
  }
  assert(p + value.size() == buf + encoded_size);
  table_.Insert(buf);
  num_entries_.fetch_add(1, std::memory_order_release);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  Table::Iterator iter(&table_);
  iter.Seek(key.memtable_key().data());
  if (!iter.Valid()) {
    return false;
  }
  // The seek landed on the first entry at or after (user key, lookup
  // sequence). It belongs to our key only if the user keys match; it is then
  // the newest version visible at the lookup sequence.
  const std::string_view internal_key = GetLengthPrefixedSlice(iter.key());
  if (comparator_.comparator.user_comparator()->Compare(ExtractUserKey(internal_key),
                                                        key.user_key()) != 0) {
    return false;
  }
  switch (static_cast<ValueType>(ExtractTag(internal_key) & 0xff)) {
    case kTypeValue: {
      const std::string_view v =
          GetLengthPrefixedSlice(internal_key.data() + internal_key.size());
      value->assign(v.data(), v.size());
      *s = Status::OK();
      return true;
    }
    case kTypeDeletion:
      *s = Status::NotFound(std::string_view());
      return true;
  }
  return false;
}

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const MemTable::Table* table) : iter_(table) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void Seek(std::string_view target) override {
    // The skiplist compares length-prefixed entries, so prefix the target.
    seek_buffer_.clear();
    PutVarint32(&seek_buffer_, static_cast<uint32_t>(target.size()));
    seek_buffer_.append(target.data(), target.size());
    iter_.Seek(seek_buffer_.data());
  }
  void Next() override { iter_.Next(); }
  std::string_view key() const override { return GetLengthPrefixedSlice(iter_.key()); }
  std::string_view value() const override {
    const std::string_view k = GetLengthPrefixedSlice(iter_.key());
    return GetLengthPrefixedSlice(k.data() + k.size());
  }
  Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator iter_;
  std::string seek_buffer_;
};

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace kvlog
