#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string_view>

#include "kvlog/env.h"
#include "kvlog/status.h"

namespace kvlog::log {

// Appends records to a log file (see log_format.h). Not thread-safe; the DB
// serializes access through its writer queue.
class Writer {
 public:
  explicit Writer(std::unique_ptr<WritableFile> dest);
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  ~Writer();

  // Appends one record whose payload is the concatenation of `pieces`, then
  // hands it to the operating system, so an acknowledged record survives a
  // crash of this process even without Sync(). On error the file may end in
  // a partial record and must not be appended to again.
  Status AddRecord(std::initializer_list<std::string_view> pieces);
  Status AddRecord(std::string_view payload) { return AddRecord({payload}); }

  // Makes every record added so far durable.
  Status Sync();

  Status Close();

  // Bytes of records successfully added (headers included).
  uint64_t size() const { return size_; }

 private:
  std::unique_ptr<WritableFile> dest_;
  uint64_t size_ = 0;
};

}  // namespace kvlog::log
