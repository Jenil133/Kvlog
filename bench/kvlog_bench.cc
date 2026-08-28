// Single-threaded throughput and latency benchmark for kvlog. Run with
// --help for the flags. Every benchmark prints one summary line; growth mode
// also writes one CSV row per step (see docs/BENCHMARKS.md).

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "kvlog/db.h"
#include "kvlog/options.h"
#include "kvlog/statistics.h"
#include "workload.h"

namespace kvlog::bench {
namespace {

constexpr const char* kUsage = R"(usage: kvlog_bench [--flag=value ...]

Benchmarks (run in order; comma separated):
  --benchmarks=fillseq,readrandom   fillseq, fillrandom, readrandom, readmissing,
                                    mixed90 (90% reads), mixed50, mixed (--read_pct)
  --num=100000           keys in the key space
  --ops=0                ops per read/mixed benchmark (0 = --num)
  --value_size=100       bytes per value
  --key_dist=uniform     uniform | zipfian (reads and mixed writes)
  --zipf_theta=0.99
  --read_pct=90          read share of "mixed" and of growth mode
  --use_existing_db=0    1 keeps the database at --db instead of recreating it
  --verify=0             1 fails if readrandom misses a key or readmissing finds one
                         (only meaningful after a full fill of --num keys)

Growth mode (instead of --benchmarks):
  --growth=0             1 grows the dataset in steps; after each step runs --ops
                         mixed ops over all keys loaded so far
  --growth_step_bytes=67108864   logical bytes (key + value) added per step
  --growth_max_bytes=1073741824  stop once the dataset reaches this size
  --csv=                 CSV output path (default: stdout)

Engine:
  --db=                  database directory (default: $TMPDIR/kvlog_bench)
  --block_cache_bytes=8388608  --bloom_bits=10  --write_buffer_size=4194304
  --sync=0               WriteOptions::sync for every write
  --seed=301
  --stats=0              1 prints the kvlog.stats property at the end
)";

struct Flags {
  std::string benchmarks = "fillseq,readrandom";
  uint64_t num = 100000;
  uint64_t ops = 0;
  uint64_t value_size = 100;
  bool zipfian = false;
  double zipf_theta = 0.99;
  int read_pct = 90;
  bool use_existing_db = false;
  bool verify = false;
  bool growth = false;
  uint64_t growth_step_bytes = 64ULL << 20;
  uint64_t growth_max_bytes = 1ULL << 30;
  std::string csv;
  std::string db;
  uint64_t block_cache_bytes = 8ULL << 20;
  int bloom_bits = 10;
  uint64_t write_buffer_size = 4ULL << 20;
  bool sync = false;
  uint64_t seed = 301;
  bool stats = false;
};

[[noreturn]] void Die(const std::string& message) {
  std::fprintf(stderr, "kvlog_bench: %s\n", message.c_str());
  std::exit(1);
}

uint64_t ParseUnsigned(std::string_view name, const std::string& text) {
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (text.empty() || *end != '\0' || text[0] == '-') {
    Die("--" + std::string(name) + " needs an unsigned number, got '" + text + "'");
  }
  return value;
}

Flags ParseFlags(int argc, char** argv) {
  Flags flags;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::fputs(kUsage, stdout);
      std::exit(0);
    }
    const size_t eq = arg.find('=');
    if (!arg.starts_with("--") || eq == std::string_view::npos) {
      Die("bad argument '" + std::string(arg) + "' (try --help)");
    }
    const std::string_view name = arg.substr(2, eq - 2);
    const std::string value(arg.substr(eq + 1));
    const auto number = [&] { return ParseUnsigned(name, value); };
    if (name == "benchmarks") {
      flags.benchmarks = value;
    } else if (name == "num") {
      flags.num = number();
    } else if (name == "ops") {
      flags.ops = number();
    } else if (name == "value_size") {
      flags.value_size = number();
    } else if (name == "key_dist") {
      if (value != "uniform" && value != "zipfian") {
        Die("--key_dist must be uniform or zipfian");
      }
      flags.zipfian = value == "zipfian";
    } else if (name == "zipf_theta") {
      flags.zipf_theta = std::strtod(value.c_str(), nullptr);
      if (!(flags.zipf_theta > 0 && flags.zipf_theta < 1)) {
        Die("--zipf_theta must be in (0, 1)");
      }
    } else if (name == "read_pct") {
      flags.read_pct = static_cast<int>(number());
      if (flags.read_pct > 100) {
        Die("--read_pct must be in [0, 100]");
      }
    } else if (name == "use_existing_db") {
      flags.use_existing_db = number() != 0;
    } else if (name == "verify") {
      flags.verify = number() != 0;
    } else if (name == "growth") {
      flags.growth = number() != 0;
    } else if (name == "growth_step_bytes") {
      flags.growth_step_bytes = number();
    } else if (name == "growth_max_bytes") {
      flags.growth_max_bytes = number();
    } else if (name == "csv") {
      flags.csv = value;
    } else if (name == "db") {
      flags.db = value;
    } else if (name == "block_cache_bytes") {
      flags.block_cache_bytes = number();
    } else if (name == "bloom_bits") {
      flags.bloom_bits = static_cast<int>(number());
    } else if (name == "write_buffer_size") {
      flags.write_buffer_size = number();
    } else if (name == "sync") {
      flags.sync = number() != 0;
    } else if (name == "seed") {
      flags.seed = number();
    } else if (name == "stats") {
      flags.stats = number() != 0;
    } else {
      Die("unknown flag --" + std::string(name) + " (try --help)");
    }
  }
  if (flags.num == 0 || flags.num > (1ULL << 32)) {
    Die("--num must be in [1, 2^32]");
  }
  if (flags.ops == 0) {
    flags.ops = flags.num;
  }
  if (flags.db.empty()) {
    const char* tmp = std::getenv("TMPDIR");
    flags.db = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") + "/kvlog_bench";
  }
  return flags;
}

void CheckOk(const Status& s, const char* what) {
  if (!s.ok()) {
    Die(std::string(what) + ": " + s.ToString());
  }
}

// Bytes of all files in the database directory (tables, logs, MANIFEST).
uint64_t DiskBytes(const std::string& dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    std::error_code size_ec;
    if (entry.is_regular_file(size_ec)) {
      const uint64_t size = entry.file_size(size_ec);
      total += size_ec ? 0 : size;
    }
  }
  return total;
}

double Ratio(uint64_t part, uint64_t whole) {
  return whole == 0 ? 0.0 : static_cast<double>(part) / static_cast<double>(whole);
}

// Counter values at the start of a measured phase.
struct CounterSnapshot {
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t bloom_checks = 0;
  uint64_t bloom_useful = 0;

  static CounterSnapshot Of(const Statistics& stats) {
    return {stats.block_cache_hits.load(), stats.block_cache_misses.load(),
            stats.bloom_checks.load(), stats.bloom_useful.load()};
  }
};

// Results of one measured phase.
struct PhaseResult {
  uint64_t ops = 0;
  uint64_t found = 0;  // Reads that returned a value.
  uint64_t reads = 0;
  double seconds = 0;
  double p50_us = 0;
  double p99_us = 0;
  double cache_hit_rate = 0;
  double bloom_useful_rate = 0;
};

class Benchmark {
 public:
  explicit Benchmark(Flags flags)
      : flags_(std::move(flags)),
        rng_(flags_.seed),
        statistics_(std::make_shared<Statistics>()) {
    // Values are slices of a random buffer: different bytes per write
    // without paying for a random generator call per byte.
    std::uniform_int_distribution<int> byte(' ', '~');
    value_source_.resize(std::max<uint64_t>(1 << 20, flags_.value_size * 4));
    for (char& c : value_source_) {
      c = static_cast<char>(byte(rng_));
    }
    write_options_.sync = flags_.sync;
  }

  void Open() {
    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = flags_.write_buffer_size;
    options.block_cache_capacity = flags_.block_cache_bytes;
    options.bloom_bits_per_key = flags_.bloom_bits;
    options.statistics = statistics_;
    if (!flags_.use_existing_db) {
      CheckOk(DestroyDB(flags_.db, options), "destroy existing database");
    }
    CheckOk(DB::Open(options, flags_.db, &db_), "open");
  }

  void PrintHeader() const {
    if (flags_.growth) {
      std::printf("kvlog_bench growth: step_bytes=%" PRIu64 " max_bytes=%" PRIu64
                  " read_pct=%d\n",
                  flags_.growth_step_bytes, flags_.growth_max_bytes, flags_.read_pct);
    }
    std::printf("kvlog_bench: db=%s num=%" PRIu64 " ops=%" PRIu64 " value_size=%" PRIu64
                " key_dist=%s block_cache_bytes=%" PRIu64 " bloom_bits=%d write_buffer_size=%" PRIu64
                " sync=%d\n",
                flags_.db.c_str(), flags_.num, flags_.ops, flags_.value_size,
                flags_.zipfian ? "zipfian" : "uniform", flags_.block_cache_bytes, flags_.bloom_bits,
                flags_.write_buffer_size, flags_.sync ? 1 : 0);
    std::fflush(stdout);
  }

  int RunList() {
    std::stringstream list(flags_.benchmarks);
    std::string name;
    while (std::getline(list, name, ',')) {
      if (name.empty()) {
        continue;
      }
      PhaseResult result;
      if (name == "fillseq") {
        result = Fill(ScatteredRange::Sequential(0, flags_.num));
      } else if (name == "fillrandom") {
        result = Fill(ScatteredRange::Scattered(0, flags_.num, flags_.seed));
      } else if (name == "readrandom") {
        result = Mixed(flags_.num, flags_.ops, 100, /*missing=*/false);
        if (flags_.verify && result.found != result.reads) {
          Report(name, result);
          Die("readrandom found " + std::to_string(result.found) + " of " +
              std::to_string(result.reads) + " keys");
        }
      } else if (name == "readmissing") {
        result = Mixed(flags_.num, flags_.ops, 100, /*missing=*/true);
        if (flags_.verify && result.found != 0) {
          Report(name, result);
          Die("readmissing found " + std::to_string(result.found) + " keys that were never written");
        }
      } else if (name == "mixed90") {
        result = Mixed(flags_.num, flags_.ops, 90, false);
      } else if (name == "mixed50") {
        result = Mixed(flags_.num, flags_.ops, 50, false);
      } else if (name == "mixed") {
        result = Mixed(flags_.num, flags_.ops, flags_.read_pct, false);
      } else {
        Die("unknown benchmark '" + name + "'");
      }
      Report(name, result);
    }
    return 0;
  }

  int RunGrowth() {
    const uint64_t entry_bytes = BenchKey(0).size() + flags_.value_size;
    const uint64_t step_keys = std::max<uint64_t>(1, flags_.growth_step_bytes / entry_bytes);
    std::FILE* out = stdout;
    if (!flags_.csv.empty()) {
      out = std::fopen(flags_.csv.c_str(), "w");
      if (out == nullptr) {
        Die("cannot write " + flags_.csv);
      }
    }
    std::fprintf(out,
                 "step,keys,dataset_bytes,disk_bytes,ops_per_sec,p50_us,p99_us,cache_hit_rate,"
                 "bloom_useful_rate,compactions,block_cache_bytes,read_pct,key_dist\n");
    uint64_t loaded = 0;
    for (int step = 1; loaded * entry_bytes < flags_.growth_max_bytes; ++step) {
      const uint64_t count = std::min<uint64_t>(step_keys, (uint64_t{1} << 32) - loaded);
      if (count == 0) {
        break;
      }
      const PhaseResult fill = Fill(ScatteredRange::Scattered(loaded, count, flags_.seed + static_cast<uint64_t>(step)));
      loaded += count;
      const PhaseResult mixed = Mixed(loaded, flags_.ops, flags_.read_pct, false);
      std::fprintf(out, "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.0f,%.2f,%.2f,%.4f,%.4f,%" PRIu64
                        ",%" PRIu64 ",%d,%s\n",
                   step, loaded, loaded * entry_bytes, DiskBytes(flags_.db),
                   static_cast<double>(mixed.ops) / mixed.seconds, mixed.p50_us, mixed.p99_us,
                   mixed.cache_hit_rate, mixed.bloom_useful_rate,
                   statistics_->compactions.load(), flags_.block_cache_bytes, flags_.read_pct,
                   flags_.zipfian ? "zipfian" : "uniform");
      std::fflush(out);
      std::fprintf(stderr, "step %d: %" PRIu64 " keys loaded at %.0f ops/s, mixed %.0f ops/s\n",
                   step, loaded, static_cast<double>(fill.ops) / fill.seconds,
                   static_cast<double>(mixed.ops) / mixed.seconds);
    }
    if (out != stdout) {
      std::fclose(out);
    }
    return 0;
  }

  const Flags& flags() const { return flags_; }

  void PrintStats() {
    std::string value;
    if (db_->GetProperty("kvlog.stats", &value)) {
      std::printf("%s", value.c_str());
    }
  }

 private:
  using Clock = std::chrono::steady_clock;

  std::string_view ValueFor() {
    const uint64_t offset =
        std::uniform_int_distribution<uint64_t>(0, value_source_.size() - flags_.value_size)(rng_);
    return std::string_view(value_source_).substr(offset, flags_.value_size);
  }

  // Writes one value for every index of `range`, in the range's order.
  PhaseResult Fill(const ScatteredRange& range) {
    const uint64_t count = range.count();
    PhaseResult result;
    latencies_.Clear();
    const CounterSnapshot before = CounterSnapshot::Of(*statistics_);
    const Clock::time_point start = Clock::now();
    for (uint64_t i = 0; i < count; ++i) {
      const std::string key = BenchKey(range.At(i));
      const Clock::time_point op_start = Clock::now();
      CheckOk(db_->Put(write_options_, key, ValueFor()), "put");
      latencies_.Add(MicrosSince(op_start));
    }
    // Background flushes and compactions triggered by the fill belong to
    // its cost; it is not finished until they are.
    db_->WaitForBackgroundWork();
    Finish(start, before, count, &result);
    return result;
  }

  // `ops` operations over keys [0, keys): `read_pct` percent Gets, the rest
  // Puts that overwrite existing keys. With `missing`, Gets look up keys
  // that were never written.
  PhaseResult Mixed(uint64_t keys, uint64_t ops, int read_pct, bool missing) {
    KeyChooser chooser = MakeChooser(keys);
    PhaseResult result;
    latencies_.Clear();
    std::uniform_int_distribution<int> percent(0, 99);
    std::string value;
    const CounterSnapshot before = CounterSnapshot::Of(*statistics_);
    const Clock::time_point start = Clock::now();
    for (uint64_t i = 0; i < ops; ++i) {
      const uint64_t index = chooser.Next(&rng_);
      const bool read = percent(rng_) < read_pct;
      const std::string key = read && missing ? MissingKey(index) : BenchKey(index);
      const Clock::time_point op_start = Clock::now();
      if (read) {
        const Status s = db_->Get(ReadOptions(), key, &value);
        if (!s.ok() && !s.IsNotFound()) {
          Die("get: " + s.ToString());
        }
        ++result.reads;
        result.found += s.ok() ? 1 : 0;
      } else {
        CheckOk(db_->Put(write_options_, key, ValueFor()), "put");
      }
      latencies_.Add(MicrosSince(op_start));
    }
    Finish(start, before, ops, &result);
    return result;
  }

  KeyChooser MakeChooser(uint64_t keys) {
    // Building a zipfian chooser costs O(keys); growth mode reuses one.
    if (!chooser_ || chooser_zipfian_ != flags_.zipfian) {
      chooser_ = std::make_unique<KeyChooser>(flags_.zipfian, keys);
      chooser_zipfian_ = flags_.zipfian;
    }
    chooser_->Grow(keys);
    return *chooser_;
  }

  static double MicrosSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
  }

  void Finish(Clock::time_point start, const CounterSnapshot& before, uint64_t ops,
              PhaseResult* result) {
    result->ops = ops;
    result->seconds = std::max(1e-9, std::chrono::duration<double>(Clock::now() - start).count());
    result->p50_us = latencies_.Percentile(50);
    result->p99_us = latencies_.Percentile(99);
    const CounterSnapshot after = CounterSnapshot::Of(*statistics_);
    const uint64_t hits = after.cache_hits - before.cache_hits;
    const uint64_t misses = after.cache_misses - before.cache_misses;
    result->cache_hit_rate = Ratio(hits, hits + misses);
    result->bloom_useful_rate =
        Ratio(after.bloom_useful - before.bloom_useful, after.bloom_checks - before.bloom_checks);
  }

  void Report(const std::string& name, const PhaseResult& r) {
    std::printf("%-12s: %10.0f ops/s  p50 %8.2f us  p99 %8.2f us  cache hit %5.1f%%  "
                "bloom useful %5.1f%%  disk %8.2f MiB  compactions %" PRIu64,
                name.c_str(), static_cast<double>(r.ops) / r.seconds, r.p50_us, r.p99_us,
                100 * r.cache_hit_rate, 100 * r.bloom_useful_rate,
                static_cast<double>(DiskBytes(flags_.db)) / (1 << 20),
                statistics_->compactions.load());
    if (r.reads > 0) {
      std::printf("  (found %" PRIu64 " of %" PRIu64 ")", r.found, r.reads);
    }
    std::printf("\n");
    std::fflush(stdout);
  }

  Flags flags_;
  std::mt19937_64 rng_;
  std::shared_ptr<Statistics> statistics_;
  std::unique_ptr<DB> db_;
  WriteOptions write_options_;
  std::string value_source_;
  LatencyRecorder latencies_;
  std::unique_ptr<KeyChooser> chooser_;
  bool chooser_zipfian_ = false;
};

}  // namespace
}  // namespace kvlog::bench

int main(int argc, char** argv) {
  kvlog::bench::Benchmark benchmark(kvlog::bench::ParseFlags(argc, argv));
  benchmark.Open();
  benchmark.PrintHeader();
  const int rc = benchmark.flags().growth ? benchmark.RunGrowth() : benchmark.RunList();
  if (benchmark.flags().stats) {
    benchmark.PrintStats();
  }
  return rc;
}
