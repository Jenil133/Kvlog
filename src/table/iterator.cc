#include "kvlog/iterator.h"

#include <cassert>
#include <utility>

#include "table/iterator_util.h"

namespace kvlog {

Iterator::~Iterator() {
  // Release in reverse order of registration, like destructors.
  for (auto it = cleanups_.rbegin(); it != cleanups_.rend(); ++it) {
    (*it)();
  }
}

void Iterator::RegisterCleanup(std::function<void()> cleanup) {
  cleanups_.push_back(std::move(cleanup));
}

namespace {

class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(Status status) : status_(std::move(status)) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view) override {}
  void Next() override { assert(false); }
  std::string_view key() const override {
    assert(false);
    return std::string_view();
  }
  std::string_view value() const override {
    assert(false);
    return std::string_view();
  }
  Status status() const override { return status_; }

 private:
  const Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewEmptyIterator() {
  return std::make_unique<EmptyIterator>(Status::OK());
}

std::unique_ptr<Iterator> NewErrorIterator(Status status) {
  return std::make_unique<EmptyIterator>(std::move(status));
}

}  // namespace kvlog
