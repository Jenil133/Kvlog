#include "table/two_level_iterator.h"

#include <cassert>
#include <string>

namespace kvlog {
namespace {

class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index_iter, BlockFunction block_function)
      : index_iter_(std::move(index_iter)), block_function_(std::move(block_function)) {}

  bool Valid() const override { return data_iter_ != nullptr && data_iter_->Valid(); }

  std::string_view key() const override {
    assert(Valid());
    return data_iter_->key();
  }
  std::string_view value() const override {
    assert(Valid());
    return data_iter_->value();
  }

  Status status() const override {
    Status s = index_iter_->status();
    if (s.ok() && data_iter_ != nullptr) {
      s = data_iter_->status();
    }
    return s;
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->SeekToFirst();
    }
    SkipExhaustedBlocks();
  }

  void Seek(std::string_view target) override {
    // The first index entry >= target names the only block that can hold
    // the first key >= target.
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_ != nullptr) {
      data_iter_->Seek(target);
    }
    SkipExhaustedBlocks();
  }

  void Next() override {
    assert(Valid());
    data_iter_->Next();
    SkipExhaustedBlocks();
  }

 private:
  // Moves to the first entry of the next non-empty block while the current
  // block has no more entries. Stops on any error.
  void SkipExhaustedBlocks() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (data_iter_ != nullptr && !data_iter_->status().ok()) {
        return;
      }
      if (!index_iter_->Valid()) {
        data_iter_.reset();
        data_handle_.clear();
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_ != nullptr) {
        data_iter_->SeekToFirst();
      }
    }
  }

  // Opens the block named by the current index entry, unless it is the one
  // already open.
  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      data_iter_.reset();
      data_handle_.clear();
      return;
    }
    const std::string_view handle = index_iter_->value();
    if (data_iter_ != nullptr && handle == data_handle_) {
      return;
    }
    data_iter_ = block_function_(handle);
    data_handle_.assign(handle.data(), handle.size());
  }

  const std::unique_ptr<Iterator> index_iter_;
  const BlockFunction block_function_;
  std::unique_ptr<Iterator> data_iter_;
  std::string data_handle_;  // Index value data_iter_ was opened from.
};

}  // namespace

std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                                              BlockFunction block_function) {
  return std::make_unique<TwoLevelIterator>(std::move(index_iter), std::move(block_function));
}

}  // namespace kvlog
