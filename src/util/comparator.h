#pragma once

#include <string_view>

namespace kvlog {

// A total order over keys. Internal only: the public API always orders user
// keys bytewise, but blocks and tables compare internal keys, so the code
// that searches them is written against this interface.
class Comparator {
 public:
  virtual ~Comparator();

  // Negative if a < b, zero if a == b, positive if a > b.
  virtual int Compare(std::string_view a, std::string_view b) const = 0;

  // Recorded in the MANIFEST so a database is never read with a different
  // order than it was written with.
  virtual const char* Name() const = 0;
};

// Unsigned lexicographic byte order. The returned object lives forever.
const Comparator* BytewiseComparator();

}  // namespace kvlog
