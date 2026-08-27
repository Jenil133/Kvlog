#pragma once

// An Env that can simulate losing power.
//
// It models what a POSIX file system guarantees: file data survives power
// loss only up to the last successful Sync() of the file, and a directory's
// entries (creates, renames, unlinks) only as of the last successful
// SyncDir() of the directory. SimulatePowerLoss() rewrites the database
// directory into exactly that durable state: files are cut back to their
// synced length, and files created, renamed or deleted since the last
// directory sync are deleted, renamed back or brought back.
//
// To crash at a chosen point, CrashAtOperation(n) makes the n-th durability
// operation (Sync, SyncDir, RenameFile, NewWritableFile, RemoveFile) and
// every mutating operation after it fail without effect, as if the machine
// had stopped right before it. The database then sees I/O errors; once it is
// closed, SimulatePowerLoss() leaves what the disk would hold.
//
// Real syncs are never issued (the model replaces them), so the tests run at
// memory speed. Only files directly inside the database directory are
// modelled; the directory itself must exist (durably) beforehand.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kvlog/env.h"
#include "test_util.h"

namespace kvlog::test {

class PowerLossEnv final : public EnvWrapper {
 public:
  // `dir` is the database directory, `scratch` an empty directory on the
  // same file system where replaced or deleted files are kept until a
  // directory sync makes their removal durable.
  PowerLossEnv(std::string dir, std::string scratch)
      : EnvWrapper(Env::Default()), dir_(std::move(dir)), scratch_(std::move(scratch)) {
    for (const std::string& name : ListDir(dir_)) {
      const std::string path = dir_ + "/" + name;
      auto inode = std::make_shared<Inode>();
      inode->path = path;
      inode->synced = FileSize(path);
      current_[name] = inode;
    }
    durable_ = current_;
  }

  // Makes operation number `n` (counting from 1) and everything after it
  // fail. 0 disables crashing.
  void CrashAtOperation(uint64_t n) { crash_at_ = n; }
  bool crashed() const { return crashed_.load(); }
  uint64_t operations() const { return operations_.load(); }

  // Rewrites the directory into its durable state. Requires that the
  // database is closed (no file of it still open for writing).
  void SimulatePowerLoss() {
    std::lock_guard<std::mutex> guard(mu_);
    // Move every file the model knows to the scratch directory, then put
    // back the durable names, each cut to its synced length.
    std::map<Inode*, std::string> staged;
    const auto stage = [&](const std::shared_ptr<Inode>& inode) {
      if (staged.contains(inode.get())) {
        return;
      }
      const std::string staged_path = scratch_ + "/staged-" + std::to_string(next_scratch_id_++);
      if (::rename(inode->path.c_str(), staged_path.c_str()) == 0) {
        staged[inode.get()] = staged_path;
      }
    };
    for (const auto& [name, inode] : current_) {
      stage(inode);
    }
    for (const auto& [name, inode] : durable_) {
      stage(inode);
    }
    for (const auto& [name, inode] : durable_) {
      const auto it = staged.find(inode.get());
      if (it == staged.end()) {
        continue;  // Never existed on disk (cannot happen for a modelled file).
      }
      const std::string path = dir_ + "/" + name;
      ::rename(it->second.c_str(), path.c_str());
      ::truncate(path.c_str(), static_cast<off_t>(inode->synced));
      inode->path = path;
      staged.erase(it);
    }
    for (const auto& [inode, path] : staged) {
      ::unlink(path.c_str());
    }
    current_ = durable_;
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    std::string name;
    if (!InDir(fname, &name)) {
      return target()->NewWritableFile(fname, result);
    }
    std::lock_guard<std::mutex> guard(mu_);
    if (!Proceed()) {
      return Crashed(fname);
    }
    Detach(name);
    std::unique_ptr<WritableFile> file;
    Status s = target()->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    auto inode = std::make_shared<Inode>();
    inode->path = fname;
    current_[name] = inode;
    *result = std::make_unique<File>(this, inode, std::move(file));
    return Status::OK();
  }

  Status RemoveFile(const std::string& fname) override {
    std::string name;
    if (!InDir(fname, &name)) {
      return target()->RemoveFile(fname);
    }
    std::lock_guard<std::mutex> guard(mu_);
    if (!current_.contains(name)) {
      return target()->RemoveFile(fname);
    }
    if (!Proceed()) {
      return Crashed(fname);
    }
    Detach(name);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target_name) override {
    std::string src_name;
    std::string target_base;
    if (!InDir(src, &src_name) || !InDir(target_name, &target_base)) {
      return target()->RenameFile(src, target_name);
    }
    std::lock_guard<std::mutex> guard(mu_);
    if (!Proceed()) {
      return Crashed(src);
    }
    const auto it = current_.find(src_name);
    if (it == current_.end()) {
      return target()->RenameFile(src, target_name);
    }
    std::shared_ptr<Inode> inode = it->second;
    Detach(target_base);
    Status s = target()->RenameFile(src, target_name);
    if (!s.ok()) {
      return s;
    }
    current_.erase(src_name);
    inode->path = target_name;
    current_[target_base] = inode;
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    if (dirname != dir_) {
      return Status::OK();  // Not modelled; the test keeps it durable.
    }
    std::lock_guard<std::mutex> guard(mu_);
    if (!Proceed()) {
      return Crashed(dirname);
    }
    durable_ = current_;
    return Status::OK();
  }

 private:
  struct Inode {
    std::string path;     // Where its bytes are now (dir_ or scratch_).
    uint64_t synced = 0;  // Length as of its last successful Sync().
  };

  class File final : public WritableFile {
   public:
    File(PowerLossEnv* env, std::shared_ptr<Inode> inode, std::unique_ptr<WritableFile> base)
        : env_(env), inode_(std::move(inode)), base_(std::move(base)) {}

    Status Append(std::string_view data) override {
      if (env_->crashed()) {
        return Status::IOError(inode_->path, "power lost");
      }
      Status s = base_->Append(data);
      if (s.ok()) {
        appended_ += data.size();
      }
      return s;
    }
    Status Flush() override {
      if (env_->crashed()) {
        return Status::IOError(inode_->path, "power lost");
      }
      return base_->Flush();
    }
    Status Sync() override {
      std::lock_guard<std::mutex> guard(env_->mu_);
      if (!env_->Proceed()) {
        return Crashed(inode_->path);
      }
      // Hand the data to the OS (so the length below is real) without a
      // real, slow sync: durability is what the model records.
      Status s = base_->Flush();
      if (s.ok()) {
        inode_->synced = appended_;
      }
      return s;
    }
    Status Close() override { return base_->Close(); }

   private:
    PowerLossEnv* const env_;
    const std::shared_ptr<Inode> inode_;
    const std::unique_ptr<WritableFile> base_;
    uint64_t appended_ = 0;
  };

  static Status Crashed(const std::string& what) { return Status::IOError(what, "power lost"); }

  bool InDir(const std::string& path, std::string* name) const {
    if (path.size() <= dir_.size() + 1 || path.compare(0, dir_.size(), dir_) != 0 ||
        path[dir_.size()] != '/' || path.find('/', dir_.size() + 1) != std::string::npos) {
      return false;
    }
    *name = path.substr(dir_.size() + 1);
    return true;
  }

  // Counts one durability operation. False once the crash point is reached.
  bool Proceed() {
    if (crashed_.load()) {
      return false;
    }
    const uint64_t n = operations_.fetch_add(1) + 1;
    if (crash_at_ != 0 && n >= crash_at_) {
      crashed_.store(true);
      return false;
    }
    return true;
  }

  // Removes `name` from the live directory. If the durable directory still
  // has the same file, its bytes are kept in the scratch directory, since
  // power loss would bring it back.
  void Detach(const std::string& name) {
    const auto it = current_.find(name);
    if (it == current_.end()) {
      return;
    }
    const std::shared_ptr<Inode> inode = it->second;
    current_.erase(it);
    bool durable = false;
    for (const auto& [durable_name, durable_inode] : durable_) {
      durable = durable || durable_inode == inode;
    }
    if (durable) {
      const std::string kept = scratch_ + "/kept-" + std::to_string(next_scratch_id_++);
      if (::rename(inode->path.c_str(), kept.c_str()) == 0) {
        inode->path = kept;
      }
    } else {
      ::unlink(inode->path.c_str());
    }
  }

  const std::string dir_;
  const std::string scratch_;
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<Inode>> current_;  // Live directory entries.
  std::map<std::string, std::shared_ptr<Inode>> durable_;  // As of the last SyncDir.
  uint64_t next_scratch_id_ = 0;
  std::atomic<uint64_t> crash_at_{0};
  std::atomic<uint64_t> operations_{0};
  std::atomic<bool> crashed_{false};
};

}  // namespace kvlog::test
