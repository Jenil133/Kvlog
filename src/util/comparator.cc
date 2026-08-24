#include "util/comparator.h"

#include <algorithm>
#include <cstring>

namespace kvlog {

Comparator::~Comparator() = default;

namespace {

class BytewiseComparatorImpl final : public Comparator {
 public:
  int Compare(std::string_view a, std::string_view b) const override {
    const size_t min_len = std::min(a.size(), b.size());
    int r = min_len == 0 ? 0 : std::memcmp(a.data(), b.data(), min_len);
    if (r == 0) {
      if (a.size() < b.size()) {
        r = -1;
      } else if (a.size() > b.size()) {
        r = 1;
      }
    }
    return r;
  }

  const char* Name() const override { return "kvlog.BytewiseComparator"; }
};

}  // namespace

const Comparator* BytewiseComparator() {
  static const BytewiseComparatorImpl comparator;
  return &comparator;
}

}  // namespace kvlog
