#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "kvlog/env.h"
#include "kvlog/status.h"

namespace kvlog::log {

// Reads records written by log::Writer, verifying both checksums of each.
//
// Reading stops at the first record that cannot be returned intact, and the
// reader then reports why (state()). The distinction that matters to
// recovery is between the two ways a log can end badly:
//
//  - kTornTail: the final record is incomplete (the file ends inside its
//    header or payload), or it fails a checksum because the file ends in a
//    run of zero bytes that starts inside it (file systems that extend a
//    file before its data reaches the disk read the missing data back as
//    zeros after power loss). This is what an append interrupted by a crash
//    leaves behind. Such a record was never acknowledged to a writer that
//    asked for durability, so dropping it is correct.
//  - kCorruption: any other record that fails a checksum, including a
//    complete final record whose bytes changed. A crash cannot produce
//    that; it means the log itself was damaged, and the record may have been
//    acknowledged (or acted on, for a MANIFEST edit), so it must not be
//    dropped silently.
//
// Records before the stopping point are always returned. Records after it
// never are: replay must not apply later writes while skipping earlier ones.
class Reader {
 public:
  enum class State {
    kReading,     // Not stopped yet.
    kEof,         // The log ends exactly after a complete record.
    kTornTail,    // See above.
    kCorruption,  // See above. status() has the details.
    kIOError,     // The file could not be read. status() has the details.
  };

  explicit Reader(std::unique_ptr<SequentialFile> file);
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Reads the next record's payload into *record. Returns false once no more
  // records can be read; state() and status() then say why.
  bool ReadRecord(std::string* record);

  State state() const { return state_; }

  // OK unless state() is kCorruption or kIOError.
  const Status& status() const { return status_; }

  // File offset just past the last record returned: the size of the valid
  // prefix of the log.
  uint64_t valid_end() const { return valid_end_; }

 private:
  // Copies up to n bytes from the file into dst. Returns fewer only at end
  // of file or on a read error (recorded in io_status_).
  size_t ReadBytes(char* dst, size_t n);
  // Refills the buffer. Returns false at end of file or on error.
  bool Fill();
  // Called after the record starting at record_offset failed a checksum,
  // with the bytes of it read so far. True if the failure is explained by a
  // zero-filled tail: the bytes from somewhere inside the record to the end
  // of the file are all zero (see the .cc for which starting points count).
  // Consumes the rest of the file.
  bool IsZeroFilledTail(uint64_t record_offset, std::string_view header,
                        std::string_view payload);
  // Consumes the rest of the file; true if it is all zero bytes.
  bool RestIsZero();
  bool Stop(State state, Status status);
  bool Corrupt(uint64_t record_offset, const char* what);

  std::unique_ptr<SequentialFile> const file_;
  std::unique_ptr<char[]> const buffer_;
  size_t pos_ = 0;  // Next unread byte in buffer_.
  size_t len_ = 0;  // Valid bytes in buffer_.
  bool eof_ = false;
  Status io_status_;
  uint64_t offset_ = 0;  // File offset of buffer_[pos_].
  uint64_t valid_end_ = 0;
  State state_ = State::kReading;
  Status status_;
};

}  // namespace kvlog::log
