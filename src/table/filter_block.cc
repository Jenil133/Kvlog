#include "table/filter_block.h"

namespace kvlog {

void FilterBlockBuilder::AddKey(std::string_view user_key) {
  if (has_last_key_ && user_key == last_key_) {
    return;
  }
  key_hashes_.push_back(BloomHash(user_key));
  last_key_.assign(user_key.data(), user_key.size());
  has_last_key_ = true;
}

std::string FilterBlockBuilder::Finish() {
  std::string result;
  BuildBloomFilter(key_hashes_, bits_per_key_, &result);
  key_hashes_.clear();
  key_hashes_.shrink_to_fit();
  return result;
}

}  // namespace kvlog
