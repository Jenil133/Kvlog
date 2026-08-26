#include "table/table_builder.h"

#include <cassert>

#include "db/dbformat.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace kvlog {

TableBuilder::TableBuilder(const Options& options, const Comparator* comparator,
                           WritableFile* file)
    : block_size_(options.block_size),
      comparator_(comparator),
      file_(file),
      data_block_(options.block_restart_interval),
      // Index entries are looked up by binary search over restart points, so
      // every index key is stored whole.
      index_block_(1) {
  if (options.bloom_bits_per_key > 0) {
    filter_block_ = std::make_unique<FilterBlockBuilder>(options.bloom_bits_per_key);
  }
}

TableBuilder::~TableBuilder() {
  assert(closed_);  // Finish() or Abandon() must have been called.
}

void TableBuilder::Add(std::string_view key, std::string_view value) {
  assert(!closed_);
  if (!status_.ok()) {
    return;
  }
  // A key out of order would silently break every binary search over the
  // finished table, so it is checked in every build, not just asserted.
  if (num_entries_ > 0 && comparator_->Compare(key, last_key_) <= 0) {
    status_ = Status::InvalidArgument("table keys added out of order");
    return;
  }
  if (filter_block_ != nullptr) {
    if (key.size() < kTagSize) {
      status_ = Status::InvalidArgument("table key is shorter than an internal key");
      return;
    }
    filter_block_->AddKey(ExtractUserKey(key));
  }
  data_block_.Add(key, value);
  last_key_.assign(key.data(), key.size());
  ++num_entries_;
  if (data_block_.CurrentSizeEstimate() >= block_size_) {
    FlushDataBlock();
  }
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty() || !status_.ok()) {
    return;
  }
  BlockHandle handle;
  WriteBlock(&data_block_, &handle);
  if (status_.ok()) {
    // The block's last key is a valid separator: it is >= every key in this
    // block and < every key in the next one.
    std::string encoded_handle;
    handle.EncodeTo(&encoded_handle);
    index_block_.Add(last_key_, encoded_handle);
  }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  WriteRawBlock(block->Finish(), handle);
  block->Reset();
}

void TableBuilder::WriteRawBlock(std::string_view contents, BlockHandle* handle) {
  *handle = BlockHandle(offset_, contents.size());
  status_ = file_->Append(contents);
  if (!status_.ok()) {
    return;
  }
  char trailer[kBlockTrailerSize];
  trailer[0] = static_cast<char>(kRawBlock);
  uint32_t crc = crc32c::Value(contents.data(), contents.size());
  crc = crc32c::Extend(crc, trailer, 1);  // Cover the storage type too.
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));
  status_ = file_->Append(std::string_view(trailer, kBlockTrailerSize));
  if (status_.ok()) {
    offset_ += contents.size() + kBlockTrailerSize;
  }
}

Status TableBuilder::Finish() {
  assert(!closed_);
  FlushDataBlock();
  closed_ = true;

  BlockHandle filter_handle;
  BlockHandle metaindex_handle;
  BlockHandle index_handle;
  if (status_.ok() && filter_block_ != nullptr) {
    WriteRawBlock(filter_block_->Finish(), &filter_handle);
  }
  if (status_.ok()) {
    BlockBuilder metaindex_block(1);
    if (filter_block_ != nullptr) {
      std::string encoded_handle;
      filter_handle.EncodeTo(&encoded_handle);
      metaindex_block.Add(kFilterBlockName, encoded_handle);
    }
    WriteBlock(&metaindex_block, &metaindex_handle);
  }
  if (status_.ok()) {
    WriteBlock(&index_block_, &index_handle);
  }
  if (status_.ok()) {
    std::string footer;
    Footer(metaindex_handle, index_handle).EncodeTo(&footer);
    status_ = file_->Append(footer);
    if (status_.ok()) {
      offset_ += footer.size();
    }
  }
  return status_;
}

void TableBuilder::Abandon() {
  assert(!closed_);
  closed_ = true;
}

}  // namespace kvlog
