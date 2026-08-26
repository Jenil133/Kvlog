#include "table/block.h"

#include <cassert>
#include <string>

#include "table/iterator_util.h"
#include "util/coding.h"

namespace kvlog {

Block::Block(BlockContents contents)
    : heap_(std::move(contents.heap)), data_(contents.data) {
  if (data_.size() < sizeof(uint32_t)) {
    malformed_ = true;
    return;
  }
  const size_t max_restarts = (data_.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  num_restarts_ = DecodeFixed32(data_.data() + data_.size() - sizeof(uint32_t));
  if (num_restarts_ > max_restarts) {
    malformed_ = true;
    return;
  }
  // The restart offsets and their count end the block.
  const size_t restart_bytes = (1 + static_cast<size_t>(num_restarts_)) * sizeof(uint32_t);
  restart_offset_ = static_cast<uint32_t>(data_.size() - restart_bytes);
}

namespace {

// Decodes the header of the entry at p. Returns a pointer to the key delta,
// or nullptr if the entry is malformed or extends beyond `limit`.
const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                        uint32_t* non_shared, uint32_t* value_length) {
  if (p >= limit || limit - p < 3) {
    return nullptr;
  }
  *shared = static_cast<unsigned char>(p[0]);
  *non_shared = static_cast<unsigned char>(p[1]);
  *value_length = static_cast<unsigned char>(p[2]);
  if ((*shared | *non_shared | *value_length) < 128) {
    p += 3;  // Fast path: all three lengths fit in one byte each.
  } else {
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == nullptr) return nullptr;
  }
  const uint64_t payload = uint64_t{*non_shared} + uint64_t{*value_length};
  if (static_cast<uint64_t>(limit - p) < payload) {
    return nullptr;
  }
  return p;
}

}  // namespace

class Block::Iter final : public Iterator {
 public:
  Iter(const Comparator* comparator, const char* data, uint32_t restarts, uint32_t num_restarts)
      : comparator_(comparator),
        data_(data),
        restarts_(restarts),
        num_restarts_(num_restarts),
        current_(restarts),
        restart_index_(num_restarts) {
    assert(num_restarts_ > 0);
  }

  bool Valid() const override { return current_ < restarts_; }
  Status status() const override { return status_; }

  std::string_view key() const override {
    assert(Valid());
    return key_;
  }
  std::string_view value() const override {
    assert(Valid());
    return value_;
  }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  void SeekToFirst() override {
    if (SeekToRestartPoint(0)) {
      ParseNextKey();
    }
  }

  void Seek(std::string_view target) override {
    // Binary search for the last restart point whose key is < target. Keys at
    // restart points are stored whole, so they can be compared directly.
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = left + (right - left + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      if (region_offset >= restarts_) {
        CorruptionError();
        return;
      }
      uint32_t shared = 0;
      uint32_t non_shared = 0;
      uint32_t value_length = 0;
      const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                                        &non_shared, &value_length);
      if (key_ptr == nullptr || shared != 0) {
        CorruptionError();
        return;
      }
      if (comparator_->Compare(std::string_view(key_ptr, non_shared), target) < 0) {
        left = mid;  // Every key before `mid` is < target as well.
      } else {
        right = mid - 1;
      }
    }
    // Scan forward from that restart point to the first key >= target.
    if (!SeekToRestartPoint(left)) {
      return;
    }
    while (ParseNextKey()) {
      if (comparator_->Compare(key_, target) >= 0) {
        return;
      }
    }
  }

 private:
  uint32_t GetRestartPoint(uint32_t index) const {
    assert(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  // Positions just before the entry at restart point `index`, so that the
  // next ParseNextKey() decodes it.
  bool SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    const uint32_t offset = GetRestartPoint(index);
    if (offset > restarts_) {
      CorruptionError();
      return false;
    }
    value_ = std::string_view(data_ + offset, 0);
    return true;
  }

  // Offset of the entry after the current one: entries are contiguous, so
  // it starts where the current value ends.
  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* const limit = data_ + restarts_;
    if (p >= limit) {
      // No more entries.
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }
    uint32_t shared = 0;
    uint32_t non_shared = 0;
    uint32_t value_length = 0;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = std::string_view(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ && GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  void CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("malformed block entry");
    key_.clear();
    value_ = std::string_view();
  }

  const Comparator* const comparator_;
  const char* const data_;
  const uint32_t restarts_;      // Offset of the restart array.
  const uint32_t num_restarts_;
  uint32_t current_;             // Offset of the current entry; restarts_ if invalid.
  uint32_t restart_index_;       // Restart block containing current_.
  std::string key_;
  std::string_view value_;
  Status status_;
};

std::unique_ptr<Iterator> Block::NewIterator(const Comparator* comparator) const {
  if (malformed_) {
    return NewErrorIterator(Status::Corruption("malformed block"));
  }
  if (num_restarts_ == 0) {
    return NewEmptyIterator();
  }
  return std::make_unique<Iter>(comparator, data_.data(), restart_offset_, num_restarts_);
}

}  // namespace kvlog
