#pragma once

#include <memory>
#include <vector>

#include "kvlog/iterator.h"
#include "util/comparator.h"

namespace kvlog {

// Returns an iterator yielding the union of `children`'s entries in
// `comparator` order: a k-way merge driven by a binary min-heap, so each step
// costs O(log k). Entries that compare equal are yielded in the order of the
// children vector, so callers list newer sources first. Duplicates are not
// removed; the DB iterator above collapses versions of the same user key.
//
// Iteration stops at the first child error, and status() reports it.
std::unique_ptr<Iterator> NewMergingIterator(const Comparator* comparator,
                                             std::vector<std::unique_ptr<Iterator>> children);

}  // namespace kvlog
