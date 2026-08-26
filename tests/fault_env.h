#pragma once

// An Env that injects I/O failures into chosen operations on chosen kinds of
// database files, and counts the operations it forwards.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kvlog/env.h"

namespace kvlog::test {

enum class FileKind { kLog = 0, kTemp = 1, kManifest = 2, kOther = 3 };
inline constexpr int kNumFileKinds = 4;

inline FileKind ClassifyFile(std::string_view path) {
  const size_t slash = path.rfind('/');
  const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (name.ends_with(".log")) return FileKind::kLog;
  if (name.ends_with(".tmp")) return FileKind::kTemp;  // Tables being built, CURRENT updates.
  if (name.starts_with("MANIFEST-")) return FileKind::kManifest;
  return FileKind::kOther;
}

inline const char* KindName(FileKind kind) {
  switch (kind) {
    case FileKind::kLog:
      return "log";
    case FileKind::kTemp:
      return "tmp";
    case FileKind::kManifest:
      return "manifest";
    case FileKind::kOther:
      break;
  }
  return "other";
}

class FaultInjectionEnv final : public EnvWrapper {
 public:
  FaultInjectionEnv() : EnvWrapper(Env::Default()) {}

  void FailAppends(FileKind kind, bool fail) { fail_append_[Index(kind)] = fail; }
  void FailSyncs(FileKind kind, bool fail) { fail_sync_[Index(kind)] = fail; }
  void FailCreates(FileKind kind, bool fail) { fail_create_[Index(kind)] = fail; }
  void FailRenames(bool fail) { fail_rename_ = fail; }
  void FailDirectorySyncs(bool fail) { fail_sync_dir_ = fail; }
  // Let the next `n` operations of the kind succeed, then fail every later
  // one (until the matching Fail*(false) call). For reaching the second step
  // of a sequence, such as the CURRENT update that follows a table install.
  void FailSyncsAfter(FileKind kind, int n) { syncs_before_failing_[Index(kind)] = n; }
  void FailRenamesAfter(int n) { renames_before_failing_ = n; }
  void FailDirectorySyncsAfter(int n) { dir_syncs_before_failing_ = n; }
  void ClearFailures() {
    for (int i = 0; i < kNumFileKinds; ++i) {
      fail_append_[i] = false;
      fail_sync_[i] = false;
      fail_create_[i] = false;
      syncs_before_failing_[i] = -1;
    }
    fail_rename_ = false;
    fail_sync_dir_ = false;
    renames_before_failing_ = -1;
    dir_syncs_before_failing_ = -1;
  }

  // Records the durability-relevant operations from now on, in order, as
  // strings such as "sync log", "create tmp", "rename tmp->sst", "syncdir",
  // "remove log" (see ClassifyFile; ".sst" names show up as "sst").
  void StartTrace() {
    std::lock_guard<std::mutex> guard(trace_mu_);
    trace_.clear();
    tracing_ = true;
  }
  std::vector<std::string> TakeTrace() {
    std::lock_guard<std::mutex> guard(trace_mu_);
    tracing_ = false;
    return std::exchange(trace_, {});
  }

  int syncs(FileKind kind) const { return syncs_[Index(kind)].load(); }
  int appends(FileKind kind) const { return appends_[Index(kind)].load(); }
  int directory_syncs() const { return directory_syncs_.load(); }
  int renames() const { return renames_.load(); }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    const FileKind kind = ClassifyFile(fname);
    if (fail_create_[Index(kind)]) {
      result->reset();
      return Status::IOError(fname, "injected create failure");
    }
    std::unique_ptr<WritableFile> file;
    Status s = target()->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    Trace(std::string("create ") + TraceName(fname));
    *result = std::make_unique<FaultyFile>(this, kind, std::move(file));
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target_name) override {
    if (fail_rename_ || CountdownExpired(&renames_before_failing_)) {
      return Status::IOError(src, "injected rename failure");
    }
    renames_++;
    Status s = target()->RenameFile(src, target_name);
    if (s.ok()) {
      Trace("rename " + TraceName(src) + "->" + TraceName(target_name));
    }
    return s;
  }

  Status RemoveFile(const std::string& fname) override {
    Status s = target()->RemoveFile(fname);
    if (s.ok()) {
      Trace(std::string("remove ") + TraceName(fname));
    }
    return s;
  }

  Status SyncDir(const std::string& dirname) override {
    if (fail_sync_dir_ || CountdownExpired(&dir_syncs_before_failing_)) {
      return Status::IOError(dirname, "injected directory sync failure");
    }
    directory_syncs_++;
    Status s = target()->SyncDir(dirname);
    if (s.ok()) {
      Trace("syncdir");
    }
    return s;
  }

 private:
  class FaultyFile final : public WritableFile {
   public:
    FaultyFile(FaultInjectionEnv* env, FileKind kind, std::unique_ptr<WritableFile> base)
        : env_(env), kind_(kind), base_(std::move(base)) {}

    Status Append(std::string_view data) override {
      if (env_->fail_append_[Index(kind_)]) {
        return Status::IOError("injected append failure");
      }
      env_->appends_[Index(kind_)]++;
      return base_->Append(data);
    }
    Status Flush() override { return base_->Flush(); }
    Status Sync() override {
      if (env_->fail_sync_[Index(kind_)] ||
          CountdownExpired(&env_->syncs_before_failing_[Index(kind_)])) {
        return Status::IOError("injected sync failure");
      }
      env_->syncs_[Index(kind_)]++;
      Status s = base_->Sync();
      if (s.ok()) {
        env_->Trace(std::string("sync ") + KindName(kind_));
      }
      return s;
    }
    Status Close() override { return base_->Close(); }

   private:
    FaultInjectionEnv* const env_;
    const FileKind kind_;
    const std::unique_ptr<WritableFile> base_;
  };

  static int Index(FileKind kind) { return static_cast<int>(kind); }

  static std::string TraceName(std::string_view path) {
    if (path.ends_with(".sst")) {
      return "sst";
    }
    if (path.ends_with("/CURRENT")) {
      return "CURRENT";
    }
    return KindName(ClassifyFile(path));
  }

  void Trace(std::string event) {
    std::lock_guard<std::mutex> guard(trace_mu_);
    if (tracing_) {
      trace_.push_back(std::move(event));
    }
  }

  // For the Fail*After countdowns: -1 means off, 0 means fail, n > 0 lets
  // this operation through and counts it.
  static bool CountdownExpired(std::atomic<int>* remaining) {
    int n = remaining->load();
    while (n > 0 && !remaining->compare_exchange_weak(n, n - 1)) {
    }
    return n == 0;
  }

  std::atomic<bool> fail_append_[kNumFileKinds] = {};
  std::atomic<bool> fail_sync_[kNumFileKinds] = {};
  std::atomic<bool> fail_create_[kNumFileKinds] = {};
  std::atomic<bool> fail_rename_{false};
  std::atomic<bool> fail_sync_dir_{false};
  std::atomic<int> syncs_before_failing_[kNumFileKinds] = {-1, -1, -1, -1};
  std::atomic<int> renames_before_failing_{-1};
  std::atomic<int> dir_syncs_before_failing_{-1};
  std::atomic<int> directory_syncs_{0};
  std::atomic<int> renames_{0};
  std::atomic<int> syncs_[kNumFileKinds] = {};
  std::mutex trace_mu_;
  bool tracing_ = false;
  std::vector<std::string> trace_;
  std::atomic<int> appends_[kNumFileKinds] = {};
};

}  // namespace kvlog::test
