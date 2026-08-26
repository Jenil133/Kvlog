#include "table/table.h"

#include <string>

#include "db/dbformat.h"
#include "table/iterator_util.h"
#include "table/two_level_iterator.h"

namespace kvlog {

Table::Table(std::unique_ptr<RandomAccessFile> file, const Comparator* comparator,
             uint64_t file_number, uint64_t file_size, std::unique_ptr<Block> index_block)
    : file_(std::move(file)),
      comparator_(comparator),
      file_number_(file_number),
      file_size_(file_size),
      index_block_(std::move(index_block)) {}

namespace {

// Prefixes the message of a failed `s` with `context`, keeping its code,
// except that a block that cannot be found is an I/O failure: NotFound
// would read as "no such key" to a caller.
Status WithContext(const Status& s, const std::string& context) {
  const std::string message = context + ": " + s.message();
  switch (s.code()) {
    case Status::Code::kOk:
      return s;
    case Status::Code::kCorruption:
      return Status::Corruption(message);
    case Status::Code::kNotFound:
    case Status::Code::kIOError:
      return Status::IOError(message);
    case Status::Code::kInvalidArgument:
      return Status::InvalidArgument(message);
    case Status::Code::kBusy:
      return Status::Busy(message);
  }
  return s;
}

}  // namespace

Status Table::Annotate(const Status& s) const {
  return WithContext(s, "table " + std::to_string(file_number_));
}

Status Table::Open(const Options& options, const Comparator* comparator,
                   std::unique_ptr<RandomAccessFile> file, uint64_t file_number,
                   uint64_t file_size, std::shared_ptr<Table>* table,
                   std::shared_ptr<Cache> block_cache) {
  table->reset();
  const std::string name = "table " + std::to_string(file_number);
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption(name, "file is too short to be a table");
  }

  char footer_space[Footer::kEncodedLength];
  std::string_view footer_input;
  Status s = file->Read(file_size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) {
    return WithContext(s, name);
  }
  if (footer_input.size() != Footer::kEncodedLength) {
    return Status::Corruption(name, "truncated footer read");
  }
  Footer footer;
  s = footer.DecodeFrom(footer_input);
  if (!s.ok()) {
    return Status::Corruption(name, s.message());
  }

  // Data blocks, metaindex and index all lie before the footer.
  const uint64_t blocks_end = file_size - Footer::kEncodedLength;
  BlockContents index_contents;
  s = ReadBlock(*file, blocks_end, footer.index_handle(), &index_contents);
  if (!s.ok()) {
    return WithContext(s, name + ": index block");
  }
  BlockContents metaindex_contents;
  s = ReadBlock(*file, blocks_end, footer.metaindex_handle(), &metaindex_contents);
  if (!s.ok()) {
    return WithContext(s, name + ": metaindex block");
  }
  // Meta blocks are found by name. The whole metaindex is walked so that
  // damage to it is reported even in entries this reader does not use;
  // unknown names are skipped (see filter_block.h). Tables written without a
  // filter have no filter entry and are read without one.
  BlockContents filter_contents;
  bool has_filter = false;
  {
    Block metaindex(std::move(metaindex_contents));
    auto iter = metaindex.NewIterator(BytewiseComparator());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      if (iter->key() != kFilterBlockName) {
        continue;
      }
      BlockHandle filter_handle;
      std::string_view handle_input = iter->value();
      s = filter_handle.DecodeFrom(&handle_input);
      if (s.ok()) {
        s = ReadBlock(*file, blocks_end, filter_handle, &filter_contents);
      }
      if (!s.ok()) {
        return WithContext(s, name + ": filter block");
      }
      has_filter = true;
    }
    if (!iter->status().ok()) {
      return Status::Corruption(name, "metaindex block: " + iter->status().message());
    }
  }

  std::shared_ptr<Table> opened(new Table(std::move(file), comparator, file_number, file_size,
                                          std::make_unique<Block>(std::move(index_contents))));
  if (has_filter) {
    opened->filter_contents_ = std::move(filter_contents);
    opened->filter_ = std::make_unique<FilterBlockReader>(opened->filter_contents_.data);
  }
  opened->block_cache_ = std::move(block_cache);
  opened->statistics_ = options.statistics;
  *table = std::move(opened);
  return Status::OK();
}

namespace {

void DeleteCachedBlock(std::string_view /*key*/, void* value) {
  delete static_cast<Block*>(value);
}

// A shared_ptr to the block a cache handle pins; dropping the last copy
// releases the handle.
std::shared_ptr<const Block> PinnedBlock(const std::shared_ptr<Cache>& cache,
                                         Cache::Handle* handle) {
  return std::shared_ptr<const Block>(static_cast<const Block*>(cache->Value(handle)),
                                      [cache, handle](const Block*) { cache->Release(handle); });
}

}  // namespace

Status Table::ReadDataBlock(const BlockHandle& handle, bool use_cache,
                            std::shared_ptr<const Block>* block) const {
  const bool cached = use_cache && block_cache_ != nullptr;
  // Block cache key: file number | block offset, both fixed64. File numbers
  // are never reused within a database, so a key never names stale bytes.
  char cache_key[16];
  if (cached) {
    EncodeFixed64(cache_key, file_number_);
    EncodeFixed64(cache_key + 8, handle.offset());
    Cache::Handle* hit = block_cache_->Lookup(std::string_view(cache_key, sizeof(cache_key)));
    if (hit != nullptr) {
      Record(&Statistics::block_cache_hits);
      *block = PinnedBlock(block_cache_, hit);
      return Status::OK();
    }
    Record(&Statistics::block_cache_misses);
  }

  BlockContents contents;
  Status s = ReadBlock(*file_, file_size_ - Footer::kEncodedLength, handle, &contents);
  if (!s.ok()) {
    return Annotate(s);
  }
  Record(&Statistics::block_reads);
  Record(&Statistics::block_read_bytes, handle.size() + kBlockTrailerSize);

  auto parsed = std::make_unique<Block>(std::move(contents));
  if (!cached) {
    *block = std::move(parsed);
    return Status::OK();
  }
  // Two threads missing on the same block both read it; the second insert
  // replaces the first, and both copies stay valid while pinned.
  const size_t charge = parsed->size() + sizeof(Block);
  Cache::Handle* inserted = block_cache_->Insert(std::string_view(cache_key, sizeof(cache_key)),
                                                 parsed.release(), charge, &DeleteCachedBlock);
  *block = PinnedBlock(block_cache_, inserted);
  return Status::OK();
}

std::unique_ptr<Iterator> Table::NewDataBlockIterator(std::string_view index_value,
                                                      bool use_cache) const {
  BlockHandle handle;
  std::string_view input = index_value;
  Status s = handle.DecodeFrom(&input);
  if (!s.ok()) {
    return NewErrorIterator(Annotate(s));
  }
  std::shared_ptr<const Block> block;
  s = ReadDataBlock(handle, use_cache, &block);
  if (!s.ok()) {
    return NewErrorIterator(std::move(s));
  }
  std::unique_ptr<Iterator> iter = block->NewIterator(comparator_);
  // The lambda's copy of the shared_ptr keeps the block alive (and pinned in
  // the cache) until the iterator is destroyed.
  iter->RegisterCleanup([block = std::move(block)] {});
  return iter;
}

std::unique_ptr<Iterator> Table::NewIterator([[maybe_unused]] const ReadOptions& options) const {
  return NewTwoLevelIterator(index_block_->NewIterator(comparator_),
                             [this](std::string_view index_value) {
                               return NewDataBlockIterator(index_value, /*use_cache=*/false);
                             });
}

Status Table::InternalGet([[maybe_unused]] const ReadOptions& options, std::string_view key,
                          const EntryCallback& on_entry) const {
  // With a filter, a lookup that finds no entry for its user key is a false
  // positive: the filter let it through although the key is not here.
  const bool filtered = filter_ != nullptr && key.size() >= kTagSize;
  std::string_view user_key;
  if (filtered) {
    user_key = ExtractUserKey(key);
    Record(&Statistics::bloom_checks);
    if (!filter_->KeyMayMatch(user_key)) {
      Record(&Statistics::bloom_useful);
      return Status::OK();
    }
  }

  std::unique_ptr<Iterator> index_iter = index_block_->NewIterator(comparator_);
  index_iter->Seek(key);
  if (!index_iter->Valid()) {
    // Either `key` sorts after the table's last key, or the index is damaged.
    if (filtered && index_iter->status().ok()) {
      Record(&Statistics::bloom_false_positives);
    }
    return Annotate(index_iter->status());
  }
  // Errors from reading the block already name this table.
  std::unique_ptr<Iterator> block_iter =
      NewDataBlockIterator(index_iter->value(), /*use_cache=*/true);
  block_iter->Seek(key);
  if (block_iter->Valid()) {
    if (filtered && (block_iter->key().size() < kTagSize ||
                     ExtractUserKey(block_iter->key()) != user_key)) {
      Record(&Statistics::bloom_false_positives);
    }
    on_entry(block_iter->key(), block_iter->value());
  } else if (filtered && block_iter->status().ok()) {
    Record(&Statistics::bloom_false_positives);
  }
  return block_iter->status();
}

}  // namespace kvlog
