// Writes to a database from a separate process, so that tests can kill it
// at arbitrary points and then check what recovery restores. Run without
// arguments for usage.

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "crash_ops.h"
#include "kvlog/db.h"
#include "kvlog/env.h"
#include "kvlog/statistics.h"
#include "test_util.h"

namespace {

constexpr const char* kUsage =
    "usage: kvlog_crash_helper [--skip-fs-sync] <mode> <db> [args]\n"
    "\n"
    "  write-then-die <db> <count> <write_buffer_size> <sync:0|1>\n"
    "      Puts a value for each of keys 0 .. count-1, then kills itself with\n"
    "      SIGKILL: no flush, no close, no destructors.\n"
    "  random-writes <db> <seed> <write_buffer_size> <sync:0|1> <ack_file> [engine]\n"
    "      Applies the op stream for <seed>. After each op is acknowledged it\n"
    "      appends 'P|D <key> <op number>' to <ack_file>, then prints\n"
    "      'ack <op number> <flushes> <compactions>' (counts finished so far\n"
    "      in this process). Runs until killed. With settle_after=<n> among\n"
    "      the settings, waits for background work to finish after op n is\n"
    "      acknowledged and before announcing it.\n"
    "  compact-and-die <db> <seed> <ops> <write_buffer_size> <stall_at> [engine]\n"
    "      Applies the first <ops> ops of the stream for <seed>, flushes, prints\n"
    "      'loaded', then compacts the whole key range in a second thread. The\n"
    "      compaction's <stall_at>-th append to an output file (counting from 1)\n"
    "      never returns; once it is reached the helper prints 'compacting\n"
    "      <file>' (or 'compaction-finished <status>' if the compaction ends\n"
    "      first), then waits for end of file on stdin; the test kills it\n"
    "      before that.\n"
    "  hold-open <db>\n"
    "      Opens the database, prints 'open', and keeps it open until stdin\n"
    "      reaches end of file.\n"
    "\n"
    "  --skip-fs-sync  use an Env whose syncs only hand data to the OS (enough\n"
    "                  for surviving a process kill, and much faster)\n"
    "  [engine]        name=value settings: l0_trigger, level_base, target_file,\n"
    "                  throttle_us, no_auto_compactions (see crash_ops.h)\n";

[[noreturn]] void Fail(const std::string& message) {
  std::fprintf(stderr, "kvlog_crash_helper: %s\n", message.c_str());
  std::exit(2);
}

[[noreturn]] void KillSelf() {
  std::fflush(stdout);
  ::kill(::getpid(), SIGKILL);
  std::abort();  // Not reached: SIGKILL cannot be caught.
}

uint64_t ParseNumber(std::string_view text) {
  uint64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || ec != std::errc() || ptr != text.data() + text.size()) {
    Fail("not a number: " + std::string(text));
  }
  return value;
}

struct Context {
  kvlog::Env* env = nullptr;
  std::string db_path;
  kvlog::test::CrashEngineConfig engine;
  uint64_t settle_after_op = 0;  // random-writes: see the usage text.
  std::shared_ptr<kvlog::Statistics> statistics = std::make_shared<kvlog::Statistics>();
};

std::unique_ptr<kvlog::DB> OpenOrFail(const Context& context, size_t write_buffer_size) {
  kvlog::Options options;
  options.create_if_missing = true;
  options.write_buffer_size = write_buffer_size;
  options.env = context.env;
  options.statistics = context.statistics;
  context.engine.ApplyTo(&options);
  std::unique_ptr<kvlog::DB> db;
  const kvlog::Status s = kvlog::DB::Open(options, context.db_path, &db);
  if (!s.ok()) {
    Fail("open: " + s.ToString());
  }
  return db;
}

void CheckOk(const kvlog::Status& s, const char* what) {
  if (!s.ok()) {
    Fail(std::string(what) + ": " + s.ToString());
  }
}

[[noreturn]] void WriteThenDie(const Context& context, uint64_t count, size_t write_buffer_size,
                               bool sync) {
  std::unique_ptr<kvlog::DB> db = OpenOrFail(context, write_buffer_size);
  kvlog::WriteOptions write_options;
  write_options.sync = sync;
  for (uint64_t i = 0; i < count; ++i) {
    CheckOk(db->Put(write_options, kvlog::test::CrashOpStream::KeyFor(i),
                    kvlog::test::CrashOpStream::ValueFor(i, 100)),
            "put");
  }
  KillSelf();
}

[[noreturn]] void RandomWrites(const Context& context, uint64_t seed, size_t write_buffer_size,
                               bool sync, const std::string& ack_file) {
  std::unique_ptr<kvlog::DB> db = OpenOrFail(context, write_buffer_size);
  const int ack_fd = ::open(ack_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (ack_fd < 0) {
    Fail("open " + ack_file + ": " + std::strerror(errno));
  }
  kvlog::WriteOptions write_options;
  write_options.sync = sync;
  kvlog::test::CrashOpStream stream(seed);
  while (true) {
    const kvlog::test::CrashOp op = stream.Next();
    if (op.is_put) {
      CheckOk(db->Put(write_options, op.key, op.value), "put");
    } else {
      CheckOk(db->Delete(write_options, op.key), "delete");
    }
    // The op is acknowledged: record it before announcing it. One write()
    // per line, so a kill leaves at worst one unterminated last line.
    const std::string line =
        std::string(op.is_put ? "P " : "D ") + op.key + " " + std::to_string(op.number) + "\n";
    if (::write(ack_fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
      Fail("write " + ack_file + ": " + std::strerror(errno));
    }
    if (op.number == context.settle_after_op) {
      db->WaitForBackgroundWork();
    }
    std::printf("ack %llu %llu %llu\n", static_cast<unsigned long long>(op.number),
                static_cast<unsigned long long>(context.statistics->flushes.load()),
                static_cast<unsigned long long>(context.statistics->compactions.load()));
    std::fflush(stdout);
  }
}

// Makes one chosen append to a table file under construction (a *.tmp file)
// never return, so the test kills the process at a point it picked rather
// than one that depends on how fast the compaction runs. Appends are counted
// only once Arm() has been called.
class StallingEnv final : public kvlog::EnvWrapper {
 public:
  explicit StallingEnv(kvlog::Env* base) : EnvWrapper(base) {}

  void Arm(uint64_t stall_at) {
    std::lock_guard<std::mutex> guard(mu_);
    stall_at_ = stall_at;
  }

  // Called when the compaction returns, which it can only do if it wrote
  // fewer appends than the stall point.
  void Finished(kvlog::Status status) {
    std::lock_guard<std::mutex> guard(mu_);
    finished_ = true;
    finished_status_ = std::move(status);
    cv_.notify_all();
  }

  // Waits until an append has stalled or the compaction has finished, and
  // returns the line to report.
  std::string WaitForStallOrFinish() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return !stalled_file_.empty() || finished_; });
    if (!stalled_file_.empty()) {
      return "compacting " + stalled_file_;
    }
    return "compaction-finished " + finished_status_.ToString();
  }

  kvlog::Status NewWritableFile(const std::string& fname,
                                std::unique_ptr<kvlog::WritableFile>* result) override {
    std::unique_ptr<kvlog::WritableFile> file;
    kvlog::Status s = target()->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    if (fname.ends_with(".tmp")) {
      const size_t slash = fname.find_last_of('/');
      file = std::make_unique<File>(this, fname.substr(slash + 1), std::move(file));
    }
    *result = std::move(file);
    return s;
  }

 private:
  class File final : public kvlog::WritableFile {
   public:
    File(StallingEnv* env, std::string name, std::unique_ptr<kvlog::WritableFile> base)
        : env_(env), name_(std::move(name)), base_(std::move(base)) {}

    kvlog::Status Append(std::string_view data) override {
      env_->MaybeStall(name_);
      return base_->Append(data);
    }
    kvlog::Status Flush() override { return base_->Flush(); }
    kvlog::Status Sync() override { return base_->Sync(); }
    kvlog::Status Close() override { return base_->Close(); }

   private:
    StallingEnv* const env_;
    const std::string name_;
    const std::unique_ptr<kvlog::WritableFile> base_;
  };

  void MaybeStall(const std::string& name) {
    std::unique_lock<std::mutex> lock(mu_);
    if (stall_at_ == 0 || ++appends_ != stall_at_) {
      return;
    }
    stalled_file_ = name;
    cv_.notify_all();
    // Blocks the compaction for good; the process ends by SIGKILL.
    while (true) {
      cv_.wait(lock);
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  uint64_t stall_at_ = 0;
  uint64_t appends_ = 0;
  std::string stalled_file_;
  bool finished_ = false;
  kvlog::Status finished_status_;
};

[[noreturn]] void CompactAndDie(Context context, uint64_t seed, uint64_t ops,
                                size_t write_buffer_size, uint64_t stall_at) {
  if (stall_at == 0) {
    Fail("stall_at counts from 1");
  }
  StallingEnv env(context.env != nullptr ? context.env : kvlog::Env::Default());
  context.env = &env;
  std::unique_ptr<kvlog::DB> db = OpenOrFail(context, write_buffer_size);
  kvlog::test::CrashOpStream stream(seed);
  for (uint64_t i = 0; i < ops; ++i) {
    const kvlog::test::CrashOp op = stream.Next();
    CheckOk(op.is_put ? db->Put(kvlog::WriteOptions(), op.key, op.value)
                      : db->Delete(kvlog::WriteOptions(), op.key),
            "write");
  }
  CheckOk(db->Flush(), "flush");
  // Nothing else writes from here on (automatic compactions are expected to
  // be off), so every table append counted after this is the compaction's.
  db->WaitForBackgroundWork();
  env.Arm(stall_at);
  std::printf("loaded\n");
  std::fflush(stdout);

  std::thread compactor([&] { env.Finished(db->CompactRange(nullptr, nullptr)); });
  compactor.detach();  // It may never return; the process ends by SIGKILL.
  std::printf("%s\n", env.WaitForStallOrFinish().c_str());
  std::fflush(stdout);
  char buf[64];
  while (::read(STDIN_FILENO, buf, sizeof(buf)) > 0) {
  }
  KillSelf();  // Never close cleanly: the test expects a crash.
}

int HoldOpen(const Context& context) {
  std::unique_ptr<kvlog::DB> db = OpenOrFail(context, 4 << 20);
  std::printf("open\n");
  std::fflush(stdout);
  char buf[64];
  while (::read(STDIN_FILENO, buf, sizeof(buf)) > 0) {
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  int arg = 1;
  kvlog::test::NoSyncEnv no_sync_env;
  Context context;
  if (arg < argc && std::string_view(argv[arg]) == "--skip-fs-sync") {
    context.env = &no_sync_env;
    ++arg;
  }
  if (argc - arg < 2) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  const std::string_view mode = argv[arg];
  context.db_path = argv[arg + 1];
  int extra = argc - arg - 2;
  char** const args = argv + arg + 2;
  // Arguments after a mode's positional ones are engine settings.
  int positional = extra;
  if (mode == "random-writes") {
    positional = 4;
  } else if (mode == "compact-and-die") {
    positional = 4;
  }
  for (int i = positional; i < extra; ++i) {
    constexpr std::string_view kSettle = "settle_after=";
    if (mode == "random-writes" && std::string_view(args[i]).starts_with(kSettle)) {
      context.settle_after_op = ParseNumber(std::string_view(args[i]).substr(kSettle.size()));
      continue;
    }
    if (!context.engine.Parse(args[i])) {
      Fail(std::string("bad engine setting: ") + args[i]);
    }
  }
  extra = std::min(extra, positional);

  if (mode == "write-then-die" && extra == 3) {
    WriteThenDie(context, ParseNumber(args[0]), ParseNumber(args[1]), ParseNumber(args[2]) != 0);
  }
  if (mode == "random-writes" && extra == 4) {
    RandomWrites(context, ParseNumber(args[0]), ParseNumber(args[1]), ParseNumber(args[2]) != 0,
                 args[3]);
  }
  if (mode == "compact-and-die" && extra == 4) {
    CompactAndDie(context, ParseNumber(args[0]), ParseNumber(args[1]), ParseNumber(args[2]),
                  ParseNumber(args[3]));
  }
  if (mode == "hold-open" && extra == 0) {
    return HoldOpen(context);
  }
  std::fputs(kUsage, stderr);
  return 2;
}
