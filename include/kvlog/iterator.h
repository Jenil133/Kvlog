#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "kvlog/status.h"

namespace kvlog {

// A forward iterator over key/value pairs in ascending key order.
//
// After any positioning call the iterator is either at an entry (Valid()) or
// exhausted. Iteration also ends early when an error is hit, so callers check
// status() after a loop to tell "no more entries" from "could not read more".
// The views returned by key() and value() stay valid until the iterator is
// moved or destroyed.
//
// One iterator must not be used by several threads at once without external
// synchronization; separate iterators are independent.
class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator();

  virtual bool Valid() const = 0;

  // Positions at the first entry, if any.
  virtual void SeekToFirst() = 0;

  // Positions at the first entry whose key is >= target.
  virtual void Seek(std::string_view target) = 0;

  // Advances to the next entry. Requires: Valid().
  virtual void Next() = 0;

  // Requires: Valid().
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  virtual Status status() const = 0;

  // Runs `cleanup` when this iterator is destroyed, after the derived class's
  // destructor. The engine uses this to pin whatever an iterator reads from
  // (memtables, table files, data blocks) for exactly as long as it exists.
  void RegisterCleanup(std::function<void()> cleanup);

 private:
  std::vector<std::function<void()>> cleanups_;
};

}  // namespace kvlog
