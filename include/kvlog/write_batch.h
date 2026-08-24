#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "kvlog/status.h"

namespace kvlog {

// An ordered group of updates applied atomically by DB::Write: after a crash
// either all of them are visible or none are. Later operations in a batch win
// over earlier ones on the same key.
class WriteBatch {
 public:
  // Receives the operations of a batch in order (see Iterate).
  class Handler {
   public:
    virtual ~Handler();
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };

  WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);

  // Appends the operations of `source` after this batch's operations.
  void Append(const WriteBatch& source);

  // Removes all operations.
  void Clear();

  // Number of operations in the batch.
  uint32_t Count() const;

  // Size of the serialized batch in bytes; it is written to the log as-is.
  size_t ApproximateSize() const { return rep_.size(); }

  // Replays the operations into `handler`. Fails with Corruption if the
  // serialized batch is malformed.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  // See write_batch.cc for the encoding.
  std::string rep_;
};

}  // namespace kvlog
