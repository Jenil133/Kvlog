#include "table/format.h"

#include <cstring>

#include "util/crc32c.h"

namespace kvlog {

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(std::string_view* input) {
  if (!GetVarint64(input, &offset_) || !GetVarint64(input, &size_)) {
    return Status::Corruption("bad block handle");
  }
  return Status::OK();
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t start = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  dst->resize(start + 2 * BlockHandle::kMaxEncodedLength);  // Zero padding.
  PutFixed32(dst, kTableFormatVersion);
  PutFixed64(dst, kTableMagicNumber);
}

Status Footer::DecodeFrom(std::string_view input) {
  if (input.size() != kEncodedLength) {
    return Status::Corruption("bad table footer size");
  }
  const char* const magic_ptr = input.data() + kEncodedLength - 8;
  if (DecodeFixed64(magic_ptr) != kTableMagicNumber) {
    return Status::Corruption("not a table file (bad magic number)");
  }
  const uint32_t version = DecodeFixed32(magic_ptr - 4);
  if (version != kTableFormatVersion) {
    return Status::Corruption("unsupported table format version", std::to_string(version));
  }
  std::string_view handles = input.substr(0, 2 * BlockHandle::kMaxEncodedLength);
  Status s = metaindex_handle_.DecodeFrom(&handles);
  if (s.ok()) {
    s = index_handle_.DecodeFrom(&handles);
  }
  return s;
}

Status ReadBlock(const RandomAccessFile& file, uint64_t file_size, const BlockHandle& handle,
                 BlockContents* result) {
  result->heap.reset();
  result->data = std::string_view();

  // Handles come from the file itself, which may be damaged: check them
  // against the file size before trusting one with an allocation.
  const uint64_t offset = handle.offset();
  const uint64_t size = handle.size();
  if (offset > file_size || size > file_size - offset ||
      kBlockTrailerSize > file_size - offset - size) {
    return Status::Corruption("block handle points outside the file");
  }

  const size_t total = static_cast<size_t>(size) + kBlockTrailerSize;
  auto buf = std::make_unique_for_overwrite<char[]>(total);
  std::string_view contents;
  Status s = file.Read(offset, total, &contents, buf.get());
  if (!s.ok()) {
    return s;
  }
  if (contents.size() != total) {
    return Status::Corruption("truncated block read");
  }
  if (contents.data() != buf.get()) {
    std::memcpy(buf.get(), contents.data(), total);
  }

  const char* const data = buf.get();
  const uint32_t expected = crc32c::Unmask(DecodeFixed32(data + size + 1));
  if (crc32c::Value(data, static_cast<size_t>(size) + 1) != expected) {
    return Status::Corruption("block checksum mismatch");
  }
  if (static_cast<uint8_t>(data[size]) != kRawBlock) {
    return Status::Corruption("unknown block storage type");
  }
  result->heap = std::move(buf);
  result->data = std::string_view(result->heap.get(), static_cast<size_t>(size));
  return Status::OK();
}

}  // namespace kvlog
