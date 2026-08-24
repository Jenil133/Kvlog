#pragma once

#include <memory>

#include "kvlog/iterator.h"
#include "kvlog/status.h"

namespace kvlog {

// An iterator with no entries.
std::unique_ptr<Iterator> NewEmptyIterator();

// An iterator with no entries whose status() is `status`. Used to report an
// error through an interface that returns an iterator.
std::unique_ptr<Iterator> NewErrorIterator(Status status);

}  // namespace kvlog
