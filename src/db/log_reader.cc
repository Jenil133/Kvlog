#include "db/log_reader.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "db/log_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace kvlog::log {
namespace {

constexpr size_t kBufferSize = 32 * 1024;

bool AllZero(const char* p, size_t n) {
  return std::all_of(p, p + n, [](char c) { return c == 0; });
}

}  // namespace

Reader::Reader(std::unique_ptr<SequentialFile> file)
    : file_(std::move(file)), buffer_(std::make_unique_for_overwrite<char[]>(kBufferSize)) {}

bool Reader::Fill() {
  if (eof_ || !io_status_.ok()) {
    return false;
  }
  std::string_view fragment;
  Status s = file_->Read(kBufferSize, &fragment, buffer_.get());
  if (!s.ok()) {
    io_status_ = std::move(s);
    return false;
  }
  if (!fragment.empty() && fragment.data() != buffer_.get()) {
    std::memmove(buffer_.get(), fragment.data(), fragment.size());
  }
  pos_ = 0;
  len_ = fragment.size();
  if (len_ < kBufferSize) {
    eof_ = true;
  }
  return len_ > 0;
}

size_t Reader::ReadBytes(char* dst, size_t n) {
  size_t copied = 0;
  while (copied < n) {
    if (pos_ == len_ && !Fill()) {
      break;
    }
    const size_t take = std::min(n - copied, len_ - pos_);
    std::memcpy(dst + copied, buffer_.get() + pos_, take);
    pos_ += take;
    copied += take;
  }
  offset_ += copied;
  return copied;
}

bool Reader::RestIsZero() {
  while (true) {
    if (pos_ == len_ && !Fill()) {
      return io_status_.ok();
    }
    if (!AllZero(buffer_.get() + pos_, len_ - pos_)) {
      return false;
    }
    offset_ += len_ - pos_;
    pos_ = len_;
  }
}

bool Reader::IsZeroFilledTail(uint64_t record_offset, std::string_view header,
                              std::string_view payload) {
  // Find where the run of zero bytes that ends the bytes read so far starts.
  const auto count_trailing_zeros = [](std::string_view bytes) {
    size_t n = 0;
    while (n < bytes.size() && bytes[bytes.size() - 1 - n] == 0) {
      ++n;
    }
    return n;
  };
  size_t zero_suffix = count_trailing_zeros(payload);  // Within header + payload.
  if (zero_suffix == payload.size()) {
    zero_suffix += count_trailing_zeros(header);
  }
  if (zero_suffix == 0) {
    return false;
  }
  const uint64_t record_bytes = header.size() + payload.size();
  const uint64_t zero_start = record_offset + record_bytes - zero_suffix;
  if (!RestIsZero()) {
    return false;
  }
  const uint64_t zero_run = offset_ - zero_start;
  // Zero fill after power loss starts where the data that reached the disk
  // ends: at a record boundary, at a sector or block boundary, or at the end
  // of an earlier partial write. A record whose content legitimately ends in
  // a few zero bytes (a WAL value, for example) and then had a bit flipped
  // must not pass for that, so a run starting elsewhere has to be long. The
  // rule is not exact: such a record whose zeros happen to start on a sector
  // boundary still passes. MANIFEST edits avoid it by always ending in a
  // non-zero byte (see version_edit.cc).
  constexpr uint64_t kSectorSize = 512;
  constexpr uint64_t kMinUnalignedZeroRun = 32;
  return zero_start == record_offset || zero_start % kSectorSize == 0 ||
         zero_run >= kMinUnalignedZeroRun;
}

bool Reader::Stop(State state, Status status) {
  state_ = state;
  status_ = std::move(status);
  return false;
}

bool Reader::Corrupt(uint64_t record_offset, const char* what) {
  return Stop(State::kCorruption,
              Status::Corruption(what, "record at offset " + std::to_string(record_offset)));
}

bool Reader::ReadRecord(std::string* record) {
  record->clear();
  if (state_ != State::kReading) {
    return false;
  }
  const uint64_t record_offset = offset_;

  char header[kHeaderSize];
  const size_t header_bytes = ReadBytes(header, kHeaderSize);
  if (!io_status_.ok()) {
    return Stop(State::kIOError, io_status_);
  }
  if (header_bytes == 0) {
    return Stop(State::kEof, Status::OK());
  }
  if (header_bytes < kHeaderSize) {
    // The writer died part way through a header.
    return Stop(State::kTornTail, Status::OK());
  }

  const uint32_t expected_header_crc = crc32c::Unmask(DecodeFixed32(header));
  if (crc32c::Value(header + 4, kHeaderSize - 4) != expected_header_crc) {
    if (IsZeroFilledTail(record_offset, std::string_view(header, kHeaderSize), {})) {
      return Stop(State::kTornTail, Status::OK());
    }
    if (!io_status_.ok()) {
      return Stop(State::kIOError, io_status_);
    }
    return Corrupt(record_offset, "log record header checksum mismatch");
  }

  const uint32_t expected_payload_crc = crc32c::Unmask(DecodeFixed32(header + 4));
  const uint32_t length = DecodeFixed32(header + 8);
  const auto type = static_cast<uint8_t>(header[12]);
  if (type == kZeroType || type > kMaxRecordType) {
    return Corrupt(record_offset, "unknown log record type");
  }

  // Read the payload in bounded chunks so that a header promising a huge
  // record at the end of a short file does not cause a huge allocation.
  constexpr size_t kChunk = 64 * 1024;
  while (record->size() < length) {
    const size_t old_size = record->size();
    const size_t want = std::min<size_t>(kChunk, length - old_size);
    record->resize(old_size + want);
    const size_t got = ReadBytes(record->data() + old_size, want);
    if (got < want) {
      record->clear();
      if (!io_status_.ok()) {
        return Stop(State::kIOError, io_status_);
      }
      // The header is intact but the file ends inside the payload.
      return Stop(State::kTornTail, Status::OK());
    }
  }

  if (crc32c::Value(record->data(), record->size()) != expected_payload_crc) {
    const bool torn = IsZeroFilledTail(record_offset, std::string_view(header, kHeaderSize), *record);
    record->clear();
    if (torn) {
      return Stop(State::kTornTail, Status::OK());
    }
    if (!io_status_.ok()) {
      return Stop(State::kIOError, io_status_);
    }
    // A complete record whose bytes changed is damage even when it is the
    // last one: an interrupted append leaves a short file or zeros, never
    // different data. Dropping it silently could drop a synced write, or a
    // MANIFEST edit that files were already deleted on the strength of.
    return Corrupt(record_offset, "log record payload checksum mismatch");
  }

  valid_end_ = offset_;
  return true;
}

}  // namespace kvlog::log
