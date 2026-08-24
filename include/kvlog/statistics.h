#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace kvlog {

// Counters a caller can attach through Options::statistics to observe what
// the database does internally. Updates use relaxed atomics: totals are exact
// once the database is idle, but a reader racing with activity may see the
// counters slightly out of step with each other.
struct Statistics {
  // Point lookups that consulted a table file's Bloom filter.
  std::atomic<uint64_t> bloom_checks{0};
  // Lookups the filter answered with "definitely absent": no index or data
  // block of that table was read.
  std::atomic<uint64_t> bloom_useful{0};
  // Lookups the filter let through although the key was not in that table.
  std::atomic<uint64_t> bloom_false_positives{0};

  // Data blocks read from table files (cache misses plus uncached reads).
  std::atomic<uint64_t> block_reads{0};
  std::atomic<uint64_t> block_read_bytes{0};
  std::atomic<uint64_t> block_cache_hits{0};
  std::atomic<uint64_t> block_cache_misses{0};

  // Memtable flushes and compactions that finished.
  std::atomic<uint64_t> flushes{0};
  std::atomic<uint64_t> compactions{0};
  std::atomic<uint64_t> compaction_bytes_read{0};
  std::atomic<uint64_t> compaction_bytes_written{0};

  // Writes delayed or blocked because level 0 had too many files.
  std::atomic<uint64_t> write_slowdowns{0};
  std::atomic<uint64_t> write_stops{0};

 private:
  template <typename Self>
  static auto CountersOf(Self& s) {
    return std::array{&s.bloom_checks,       &s.bloom_useful,          &s.bloom_false_positives,
                      &s.block_reads,        &s.block_read_bytes,      &s.block_cache_hits,
                      &s.block_cache_misses, &s.flushes,               &s.compactions,
                      &s.compaction_bytes_read, &s.compaction_bytes_written,
                      &s.write_slowdowns,    &s.write_stops};
  }

 public:
  void Reset() {
    for (auto* counter : CountersOf(*this)) counter->store(0, std::memory_order_relaxed);
  }

  // One "name value" pair per line, in declaration order.
  std::string ToString() const {
    static constexpr std::array<const char*, 13> kNames = {
        "bloom_checks",          "bloom_useful",       "bloom_false_positives",
        "block_reads",           "block_read_bytes",   "block_cache_hits",
        "block_cache_misses",    "flushes",            "compactions",
        "compaction_bytes_read", "compaction_bytes_written",
        "write_slowdowns",       "write_stops"};
    const auto counters = CountersOf(*this);
    std::string out;
    for (size_t i = 0; i < counters.size(); ++i) {
      out += kNames[i];
      out += ' ';
      out += std::to_string(counters[i]->load(std::memory_order_relaxed));
      out += '\n';
    }
    return out;
  }
};

}  // namespace kvlog
