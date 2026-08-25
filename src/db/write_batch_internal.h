#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "db/dbformat.h"
#include "kvlog/status.h"
#include "kvlog/write_batch.h"

namespace kvlog {

class MemTable;

// Access to the serialized form of a WriteBatch, which is also the payload of
// a write-ahead log record.
class WriteBatchInternal {
 public:
  // fixed64 sequence number + fixed32 count.
  static constexpr size_t kHeaderSize = 12;

  static uint32_t Count(const WriteBatch& batch);
  static void SetCount(WriteBatch* batch, uint32_t count);

  // Sequence number of the batch's first operation; the others follow it
  // consecutively.
  static SequenceNumber Sequence(const WriteBatch& batch);
  static void SetSequence(WriteBatch* batch, SequenceNumber sequence);

  static std::string_view Contents(const WriteBatch& batch) { return batch.rep_; }
  static size_t ByteSize(const WriteBatch& batch) { return batch.rep_.size(); }

  // Replaces the batch with a serialized batch read back from the log.
  // Requires: contents.size() >= kHeaderSize.
  static void SetContents(WriteBatch* batch, std::string_view contents);

  // Checks that the serialized batch is well formed (as Iterate does)
  // without applying it anywhere.
  static Status Validate(const WriteBatch& batch);

  // Inserts the batch's operations into `memtable`, numbering them
  // consecutively from `first_sequence`. The batch's own sequence field is
  // ignored, so a const batch can be applied with a sequence assigned at
  // commit time.
  static Status InsertInto(const WriteBatch& batch, SequenceNumber first_sequence,
                           MemTable* memtable);

  // Appends the operations of `src` to `dst`.
  static void Append(WriteBatch* dst, const WriteBatch& src);
};

}  // namespace kvlog
