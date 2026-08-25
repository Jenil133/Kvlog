#include "db/log_writer.h"

#include "db/log_format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace kvlog::log {

Writer::Writer(std::unique_ptr<WritableFile> dest) : dest_(std::move(dest)) {}

Writer::~Writer() = default;

Status Writer::AddRecord(std::initializer_list<std::string_view> pieces) {
  uint64_t length = 0;
  uint32_t payload_crc = 0;
  for (const std::string_view piece : pieces) {
    length += piece.size();
    payload_crc = crc32c::Extend(payload_crc, piece.data(), piece.size());
  }
  if (length > kMaxPayloadSize) {
    return Status::InvalidArgument("log record too large", std::to_string(length));
  }

  char header[kHeaderSize];
  EncodeFixed32(header + 4, crc32c::Mask(payload_crc));
  EncodeFixed32(header + 8, static_cast<uint32_t>(length));
  header[12] = static_cast<char>(kFullType);
  EncodeFixed32(header, crc32c::Mask(crc32c::Value(header + 4, kHeaderSize - 4)));

  Status s = dest_->Append(std::string_view(header, kHeaderSize));
  for (const std::string_view piece : pieces) {
    if (!s.ok()) {
      break;
    }
    s = dest_->Append(piece);
  }
  if (s.ok()) {
    s = dest_->Flush();
  }
  if (s.ok()) {
    size_ += kHeaderSize + length;
  }
  return s;
}

Status Writer::Sync() { return dest_->Sync(); }

Status Writer::Close() { return dest_->Close(); }

}  // namespace kvlog::log
