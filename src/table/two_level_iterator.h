#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "kvlog/iterator.h"

namespace kvlog {

// Opens the second-level iterator named by an index entry's value: a data
// block for a table's index, or a table file for a level's file list.
using BlockFunction = std::function<std::unique_ptr<Iterator>(std::string_view index_value)>;

// Returns an iterator that walks `index_iter` and, for each index entry, the
// entries of the iterator `block_function` opens for it, presenting them as
// one sorted sequence. Requires that every index key is >= all keys of its
// block and < all keys of the next block (true for "last key in block").
//
// Iteration stops at the first block that fails to open or read, and
// status() reports why; it never skips a damaged block silently.
std::unique_ptr<Iterator> NewTwoLevelIterator(std::unique_ptr<Iterator> index_iter,
                                              BlockFunction block_function);

}  // namespace kvlog
