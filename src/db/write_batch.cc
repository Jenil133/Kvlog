// Serialized form of a WriteBatch (and of every write-ahead log payload):
//
//   batch  := sequence (fixed64) | count (fixed32) | record*
//   record := kTypeValue varstring varstring     (put: key, value)
//           | kTypeDeletion varstring            (delete: key)
//   varstring := length (varint32) | bytes
//
// `sequence` is the sequence number of the first record; it is filled in when
// the batch is committed and is meaningful only in the log.

#include "kvlog/write_batch.h"

#include <cassert>

#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"

namespace kvlog {

WriteBatch::Handler::~Handler() = default;

WriteBatch::WriteBatch() { Clear(); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(WriteBatchInternal::kHeaderSize);
}

uint32_t WriteBatch::Count() const { return WriteBatchInternal::Count(*this); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(*this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(*this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixed(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) { WriteBatchInternal::Append(this, source); }

Status WriteBatch::Iterate(Handler* handler) const {
  std::string_view input(rep_);
  if (input.size() < WriteBatchInternal::kHeaderSize) {
    return Status::Corruption("malformed write batch", "too small");
  }
  input.remove_prefix(WriteBatchInternal::kHeaderSize);

  uint32_t found = 0;
  while (!input.empty()) {
    const auto tag = static_cast<uint8_t>(input.front());
    input.remove_prefix(1);
    std::string_view key;
    std::string_view value;
    switch (tag) {
      case kTypeValue:
        if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
          return Status::Corruption("malformed write batch", "bad put record");
        }
        handler->Put(key, value);
        break;
      case kTypeDeletion:
        if (!GetLengthPrefixed(&input, &key)) {
          return Status::Corruption("malformed write batch", "bad delete record");
        }
        handler->Delete(key);
        break;
      default:
        return Status::Corruption("malformed write batch", "unknown record tag");
    }
    ++found;
  }
  if (found != WriteBatchInternal::Count(*this)) {
    return Status::Corruption("malformed write batch", "record count mismatch");
  }
  return Status::OK();
}

uint32_t WriteBatchInternal::Count(const WriteBatch& batch) {
  return DecodeFixed32(batch.rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* batch, uint32_t count) {
  EncodeFixed32(batch->rep_.data() + 8, count);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch& batch) {
  return DecodeFixed64(batch.rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* batch, SequenceNumber sequence) {
  EncodeFixed64(batch->rep_.data(), sequence);
}

void WriteBatchInternal::SetContents(WriteBatch* batch, std::string_view contents) {
  assert(contents.size() >= kHeaderSize);
  batch->rep_.assign(contents.data(), contents.size());
}

namespace {

class NullHandler final : public WriteBatch::Handler {
 public:
  void Put(std::string_view, std::string_view) override {}
  void Delete(std::string_view) override {}
};

class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber first_sequence, MemTable* memtable)
      : sequence_(first_sequence), memtable_(memtable) {}

  void Put(std::string_view key, std::string_view value) override {
    memtable_->Add(sequence_++, kTypeValue, key, value);
  }
  void Delete(std::string_view key) override {
    memtable_->Add(sequence_++, kTypeDeletion, key, std::string_view());
  }

 private:
  SequenceNumber sequence_;
  MemTable* const memtable_;
};

}  // namespace

Status WriteBatchInternal::Validate(const WriteBatch& batch) {
  NullHandler handler;
  return batch.Iterate(&handler);
}

Status WriteBatchInternal::InsertInto(const WriteBatch& batch, SequenceNumber first_sequence,
                                      MemTable* memtable) {
  MemTableInserter inserter(first_sequence, memtable);
  return batch.Iterate(&inserter);
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch& src) {
  assert(src.rep_.size() >= kHeaderSize);
  SetCount(dst, Count(*dst) + Count(src));
  dst->rep_.append(src.rep_.data() + kHeaderSize, src.rep_.size() - kHeaderSize);
}

}  // namespace kvlog
