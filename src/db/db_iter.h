#pragma once

#include <memory>

#include "db/dbformat.h"
#include "kvlog/iterator.h"

namespace kvlog {

// Wraps an iterator over internal keys (the merge of memtables and tables)
// and presents user keys as of snapshot `sequence`: entries newer than the
// snapshot are skipped, only the newest visible version of each user key is
// yielded, and keys whose newest visible version is a tombstone are hidden.
std::unique_ptr<Iterator> NewDBIterator(std::unique_ptr<Iterator> internal_iter,
                                        const Comparator* user_comparator,
                                        SequenceNumber sequence);

}  // namespace kvlog
