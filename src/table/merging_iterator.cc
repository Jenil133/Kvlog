#include "table/merging_iterator.h"

#include <algorithm>
#include <cassert>
#include <cstddef>

#include "table/iterator_util.h"

namespace kvlog {
namespace {

class MergingIterator final : public Iterator {
 public:
  MergingIterator(const Comparator* comparator, std::vector<std::unique_ptr<Iterator>> children)
      : comparator_(comparator), children_(std::move(children)), heap_order_{this} {
    heap_.reserve(children_.size());
  }

  bool Valid() const override { return status_.ok() && !heap_.empty(); }

  std::string_view key() const override {
    assert(Valid());
    return children_[heap_.front()]->key();
  }
  std::string_view value() const override {
    assert(Valid());
    return children_[heap_.front()]->value();
  }

  Status status() const override { return status_; }

  void SeekToFirst() override {
    for (auto& child : children_) {
      child->SeekToFirst();
    }
    RebuildHeap();
  }

  void Seek(std::string_view target) override {
    for (auto& child : children_) {
      child->Seek(target);
    }
    RebuildHeap();
  }

  void Next() override {
    assert(Valid());
    // Take the smallest child off the heap, advance it, and put it back if
    // it still has entries.
    std::pop_heap(heap_.begin(), heap_.end(), heap_order_);
    const size_t index = heap_.back();
    heap_.pop_back();
    children_[index]->Next();
    if (children_[index]->Valid()) {
      heap_.push_back(index);
      std::push_heap(heap_.begin(), heap_.end(), heap_order_);
    } else {
      NoteError(index);
    }
  }

 private:
  // Heap order: the std heap algorithms keep the *largest* element (by this
  // predicate) at the front, so "a sorts after b" puts the smallest key on
  // top. Ties go to the lower child index, i.e. the newer source.
  struct HeapOrder {
    const MergingIterator* self;
    bool operator()(size_t a, size_t b) const {
      const int r =
          self->comparator_->Compare(self->children_[a]->key(), self->children_[b]->key());
      if (r != 0) {
        return r > 0;
      }
      return a > b;
    }
  };

  void RebuildHeap() {
    status_ = Status::OK();
    heap_.clear();
    for (size_t i = 0; i < children_.size(); ++i) {
      if (children_[i]->Valid()) {
        heap_.push_back(i);
      } else {
        NoteError(i);
      }
    }
    std::make_heap(heap_.begin(), heap_.end(), heap_order_);
  }

  // Records the error of an exhausted child, if it stopped because of one.
  void NoteError(size_t index) {
    if (status_.ok()) {
      status_ = children_[index]->status();
    }
  }

  const Comparator* const comparator_;
  const std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<size_t> heap_;  // Indexes of the valid children.
  const HeapOrder heap_order_;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children) {
  if (children.empty()) {
    return NewEmptyIterator();
  }
  if (children.size() == 1) {
    return std::move(children.front());
  }
  return std::make_unique<MergingIterator>(comparator, std::move(children));
}

}  // namespace kvlog
