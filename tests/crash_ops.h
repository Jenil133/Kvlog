#pragma once

// The stream of writes performed by the crash helper, shared with the tests
// that check what survived. For a given seed, op n is always the same, so a
// test can tell exactly which write was in flight when the helper died.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "kvlog/options.h"

namespace kvlog::test {

struct CrashOp {
  uint64_t number = 0;  // Position in the stream, starting at 1.
  bool is_put = false;
  std::string key;
  std::string value;  // Empty for deletes.
};

class CrashOpStream {
 public:
  // Keys are drawn from a small space so that keys are overwritten and
  // deleted many times over.
  static constexpr uint64_t kKeySpace = 500;

  explicit CrashOpStream(uint64_t seed) : rng_(seed) {}

  static std::string KeyFor(uint64_t index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "crash%04llu", static_cast<unsigned long long>(index));
    return buf;
  }

  // "op<number>." followed by `padding` filler bytes.
  static std::string ValueFor(uint64_t number, size_t padding) {
    return "op" + std::to_string(number) + "." + std::string(padding, 'x');
  }

  // Recovers the op number from a value written by the stream.
  static bool ParseValue(std::string_view value, uint64_t* number) {
    if (!value.starts_with("op")) {
      return false;
    }
    const char* const begin = value.data() + 2;
    const char* const end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, *number);
    return ec == std::errc() && ptr != begin && ptr != end && *ptr == '.';
  }

  CrashOp Next() {
    CrashOp op;
    op.number = ++count_;
    op.key = KeyFor(rng_() % kKeySpace);
    op.is_put = rng_() % 4 != 0;
    const size_t padding = static_cast<size_t>(rng_() % 200);
    if (op.is_put) {
      op.value = ValueFor(op.number, padding);
    }
    return op;
  }

 private:
  // The raw engine output is specified by the standard, unlike the
  // distributions, so the stream is the same on every platform.
  std::mt19937_64 rng_;
  uint64_t count_ = 0;
};

// Engine settings the crash helper opens its database with, beyond the write
// buffer size. They travel on the helper's command line as name=value
// arguments so the test can open the recovered database the same way.
// Defaults are the library defaults: a run only changes what it sets.
struct CrashEngineConfig {
  int level0_compaction_trigger = Options().level0_compaction_trigger;
  uint64_t max_bytes_for_level_base = Options().max_bytes_for_level_base;
  uint64_t target_file_size = Options().target_file_size;
  uint32_t compaction_throttle_us = Options().compaction_throttle_us;
  bool disable_auto_compactions = Options().disable_auto_compactions;

  // Tiny thresholds: with a 16 KiB write buffer (about a hundred ops of the
  // crash op stream per flush), level 0 compacts every couple of flushes and
  // the live data (about 60 KiB) overflows level 1 into level 2.
  static CrashEngineConfig CompactConstantly() {
    CrashEngineConfig config;
    config.level0_compaction_trigger = 2;
    config.max_bytes_for_level_base = 24 << 10;
    config.target_file_size = 8 << 10;
    return config;
  }

  void ApplyTo(Options* options) const {
    options->level0_compaction_trigger = level0_compaction_trigger;
    options->max_bytes_for_level_base = max_bytes_for_level_base;
    options->target_file_size = target_file_size;
    options->compaction_throttle_us = compaction_throttle_us;
    options->disable_auto_compactions = disable_auto_compactions;
  }

  std::vector<std::string> ToArgs() const {
    return {"l0_trigger=" + std::to_string(level0_compaction_trigger),
            "level_base=" + std::to_string(max_bytes_for_level_base),
            "target_file=" + std::to_string(target_file_size),
            "throttle_us=" + std::to_string(compaction_throttle_us),
            std::string("no_auto_compactions=") + (disable_auto_compactions ? "1" : "0")};
  }

  // Applies one name=value argument. Returns false for anything else.
  bool Parse(std::string_view arg) {
    const size_t eq = arg.find('=');
    if (eq == std::string_view::npos) {
      return false;
    }
    const std::string_view name = arg.substr(0, eq);
    const std::string_view text = arg.substr(eq + 1);
    uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc() || ptr != text.data() + text.size()) {
      return false;
    }
    if (name == "l0_trigger") {
      level0_compaction_trigger = static_cast<int>(value);
    } else if (name == "level_base") {
      max_bytes_for_level_base = value;
    } else if (name == "target_file") {
      target_file_size = value;
    } else if (name == "throttle_us") {
      compaction_throttle_us = static_cast<uint32_t>(value);
    } else if (name == "no_auto_compactions") {
      disable_auto_compactions = value != 0;
    } else {
      return false;
    }
    return true;
  }
};

}  // namespace kvlog::test
