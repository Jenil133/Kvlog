#include "db/db_iter.h"

#include <cassert>
#include <string>

namespace kvlog {
namespace {

class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> iter, const Comparator* user_comparator,
         SequenceNumber sequence)
      : iter_(std::move(iter)), user_comparator_(user_comparator), sequence_(sequence) {}

  bool Valid() const override { return valid_; }

  std::string_view key() const override {
    assert(valid_);
    return ExtractUserKey(iter_->key());
  }
  std::string_view value() const override {
    assert(valid_);
    return iter_->value();
  }

  Status status() const override {
    if (!status_.ok()) {
      return status_;
    }
    return iter_->status();
  }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(/*skipping=*/false);
  }

  void Seek(std::string_view target) override {
    // (target, snapshot sequence) sorts before every version of `target`
    // visible in the snapshot and after the ones written later.
    seek_key_.clear();
    AppendInternalKey(&seek_key_, ParsedInternalKey(target, sequence_, kValueTypeForSeek));
    iter_->Seek(seek_key_);
    FindNextUserEntry(/*skipping=*/false);
  }

  void Next() override {
    assert(valid_);
    // Older versions of the current key follow it directly; skip them all.
    const std::string_view current = key();
    skip_key_.assign(current.data(), current.size());
    iter_->Next();
    FindNextUserEntry(/*skipping=*/true);
  }

 private:
  // Advances iter_ to the newest visible version of the next user key that
  // is not deleted. With `skipping`, entries for user keys <= skip_key_ are
  // hidden (they are older versions of a key already yielded or deleted).
  void FindNextUserEntry(bool skipping) {
    for (; iter_->Valid(); iter_->Next()) {
      ParsedInternalKey ikey;
      if (!ParseInternalKey(iter_->key(), &ikey)) {
        status_ = Status::Corruption("corrupted internal key in iterator");
        valid_ = false;
        return;
      }
      if (ikey.sequence > sequence_) {
        continue;  // Written after the snapshot was taken.
      }
      if (skipping && user_comparator_->Compare(ikey.user_key, skip_key_) <= 0) {
        continue;  // Shadowed by a newer version.
      }
      if (ikey.type == kTypeDeletion) {
        // The key is deleted as of the snapshot; hide all its older versions.
        skip_key_.assign(ikey.user_key.data(), ikey.user_key.size());
        skipping = true;
        continue;
      }
      valid_ = true;
      return;
    }
    valid_ = false;
  }

  const std::unique_ptr<Iterator> iter_;
  const Comparator* const user_comparator_;
  const SequenceNumber sequence_;
  bool valid_ = false;
  Status status_;
  std::string skip_key_;
  std::string seek_key_;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal_iter,
                                        const Comparator* user_comparator,
                                        SequenceNumber sequence) {
  return std::make_unique<DBIter>(std::move(internal_iter), user_comparator, sequence);
}

}  // namespace kvlog
