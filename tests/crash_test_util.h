#pragma once

// Running the crash helper and checking what survives its death. Test
// binaries that include this define KVLOG_CRASH_HELPER_PATH.

#include <signal.h>
#include <sys/wait.h>

#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "crash_ops.h"
#include "kvlog/db.h"
#include "subprocess.h"
#include "test_util.h"

namespace kvlog::test {

struct CrashRun {
  uint64_t seed = 1;
  size_t write_buffer_size = 16 << 10;  // Small, so flushes run constantly.
  bool sync = false;                    // WriteOptions::sync for every op.
  bool skip_fs_sync = true;             // See NoSyncEnv.
  uint64_t kill_after_acks = 1000;
  // If not 0, the helper waits for its background flushes and compactions
  // to finish after acknowledging this op and before announcing it. Makes
  // "the helper finished a flush / compaction before the kill" a property
  // of the data rather than of scheduling.
  uint64_t settle_after_op = 0;
  CrashEngineConfig engine;             // Compaction settings of the helper.
};

// What the helper reported before it died.
struct CrashRunReport {
  uint64_t acks = 0;         // "ack" lines read before the kill.
  uint64_t flushes = 0;      // Flushes the helper had finished by its last ack.
  uint64_t compactions = 0;  // Compactions likewise.
};

// Runs the helper's random-writes mode on `dbname` and kills it with SIGKILL
// as soon as it has announced `run.kill_after_acks` acknowledged ops. It may
// acknowledge a few more before the signal lands; `ack_file` has them all.
inline void RunRandomWritesAndKill(const std::string& dbname, const std::string& ack_file,
                                   const CrashRun& run, CrashRunReport* report = nullptr) {
  std::vector<std::string> argv = {KVLOG_CRASH_HELPER_PATH};
  if (run.skip_fs_sync) {
    argv.emplace_back("--skip-fs-sync");
  }
  for (const std::string& arg :
       {std::string("random-writes"), dbname, std::to_string(run.seed),
        std::to_string(run.write_buffer_size), std::string(run.sync ? "1" : "0"), ack_file}) {
    argv.push_back(arg);
  }
  for (std::string& arg : run.engine.ToArgs()) {
    argv.push_back(std::move(arg));
  }
  if (run.settle_after_op != 0) {
    argv.push_back("settle_after=" + std::to_string(run.settle_after_op));
  }
  Subprocess child;
  ASSERT_TRUE(child.Start(argv));
  CrashRunReport seen;
  std::string line;
  while (seen.acks < run.kill_after_acks && child.ReadLine(&line)) {
    if (line.starts_with("ack ")) {
      ++seen.acks;
      std::istringstream fields(line.substr(4));
      uint64_t number = 0;
      fields >> number >> seen.flushes >> seen.compactions;
    }
  }
  child.Kill(SIGKILL);
  const int status = child.Wait();
  if (report != nullptr) {
    *report = seen;
  }
  ASSERT_EQ(run.kill_after_acks, seen.acks) << "the helper stopped early";
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) << "wait status " << status;
}

// Checks a database recovered after the helper died during random-writes
// with `seed`, starting from contents `base`. Every key must hold the
// result of its last acknowledged op (or its `base` value if the run never
// acknowledged an op on it), except that the one op in flight at the kill,
// the op after the last acknowledged one, may or may not have been applied.
// Nothing else may appear. Returns the number of acknowledged ops.
inline uint64_t VerifyAgainstAcks(DB* db, uint64_t seed, const std::string& ack_file,
                                  const std::map<std::string, std::string>& base = {}) {
  const std::string acks = ReadFileBytes(ack_file);
  uint64_t acked = 0;
  std::map<std::string, uint64_t> last_op;  // Key -> its last acknowledged op.
  for (size_t pos = 0;;) {
    const size_t newline = acks.find('\n', pos);
    if (newline == std::string::npos) {
      break;  // A final line without its newline was cut off by the kill.
    }
    std::istringstream fields(acks.substr(pos, newline - pos));
    char kind = 0;
    std::string key;
    uint64_t number = 0;
    EXPECT_TRUE(fields >> kind >> key >> number) << "bad ack line at offset " << pos;
    EXPECT_EQ(acked + 1, number) << "acks out of order";
    acked = number;
    last_op[key] = number;
    pos = newline + 1;
  }

  CrashOpStream stream(seed);
  std::vector<CrashOp> ops(acked + 2);
  for (uint64_t n = 1; n <= acked + 1; ++n) {
    ops[n] = stream.Next();
  }
  const CrashOp& in_flight = ops[acked + 1];

  std::map<std::string, std::string> scanned = ScanAll(db);
  for (uint64_t index = 0; index < CrashOpStream::kKeySpace; ++index) {
    const std::string key = CrashOpStream::KeyFor(index);
    std::optional<std::string> expected;  // nullopt: the key must be absent.
    if (const auto it = last_op.find(key); it != last_op.end()) {
      if (ops[it->second].is_put) {
        expected = ops[it->second].value;
      }
    } else if (const auto b = base.find(key); b != base.end()) {
      expected = b->second;
    }
    std::optional<std::string> alternative = expected;
    if (in_flight.key == key) {
      alternative = in_flight.is_put ? std::optional<std::string>(in_flight.value) : std::nullopt;
    }

    std::string value;
    const Status s = db->Get(ReadOptions(), key, &value);
    EXPECT_TRUE(s.ok() || s.IsNotFound()) << key << ": " << s.ToString();
    const std::optional<std::string> actual =
        s.ok() ? std::optional<std::string>(value) : std::nullopt;
    EXPECT_TRUE(actual == expected || actual == alternative)
        << key << " holds " << actual.value_or("<absent>") << "; expected "
        << expected.value_or("<absent>") << " or " << alternative.value_or("<absent>") << " ("
        << acked << " ops acknowledged)";

    const auto in_scan = scanned.find(key);
    EXPECT_EQ(actual.has_value(), in_scan != scanned.end()) << key;
    if (actual.has_value() && in_scan != scanned.end()) {
      EXPECT_EQ(*actual, in_scan->second) << key;
    }
    if (in_scan != scanned.end()) {
      scanned.erase(in_scan);
    }
  }
  EXPECT_TRUE(scanned.empty()) << scanned.size() << " keys that were never written";
  return acked;
}

// The contents a database must hold after the first `ops` ops of the stream
// for `seed`, applied to `base`.
inline std::map<std::string, std::string> ModelAfterOps(
    uint64_t seed, uint64_t ops, std::map<std::string, std::string> base = {}) {
  CrashOpStream stream(seed);
  for (uint64_t i = 0; i < ops; ++i) {
    const CrashOp op = stream.Next();
    if (op.is_put) {
      base[op.key] = op.value;
    } else {
      base.erase(op.key);
    }
  }
  return base;
}

// Runs the helper's compact-and-die mode on `dbname` and kills it with
// SIGKILL while the manual compaction is stuck in its `stall_at`-th append
// to an output file (the helper makes that append block until the kill).
// Returns true if the kill landed there, false if the compaction finished
// with fewer appends (then the kill tests a crash after a completed
// compaction). The data, and so the outcome, is the same on every run.
inline bool RunCompactAndKill(const std::string& dbname, uint64_t seed, uint64_t ops,
                              size_t write_buffer_size, const CrashEngineConfig& engine,
                              uint64_t stall_at) {
  std::vector<std::string> argv = {KVLOG_CRASH_HELPER_PATH, "--skip-fs-sync", "compact-and-die",
                                   dbname, std::to_string(seed), std::to_string(ops),
                                   std::to_string(write_buffer_size), std::to_string(stall_at)};
  for (std::string& arg : engine.ToArgs()) {
    argv.push_back(std::move(arg));
  }
  Subprocess child;
  EXPECT_TRUE(child.Start(argv));
  bool loaded = false;
  bool inside = false;
  std::string line;
  while (child.ReadLine(&line)) {
    if (line == "loaded") {
      loaded = true;
    } else if (line.starts_with("compacting ")) {
      inside = true;
      break;
    } else if (line.starts_with("compaction-finished ")) {
      EXPECT_EQ("compaction-finished OK", line);
      break;
    }
  }
  child.Kill(SIGKILL);
  const int status = child.Wait();
  EXPECT_TRUE(loaded) << "the helper did not load its data";
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) << "wait status " << status;
  return inside;
}

}  // namespace kvlog::test
