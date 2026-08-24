#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kvlog/status.h"

namespace kvlog {

// A file read from start to end (log and MANIFEST replay).
class SequentialFile {
 public:
  SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
  virtual ~SequentialFile();

  // Reads up to n bytes. *result points at the bytes read, which may be
  // stored in `scratch` (at least n bytes long). Returns fewer than n bytes
  // only at end of file.
  virtual Status Read(size_t n, std::string_view* result, char* scratch) = 0;
};

// A file read at arbitrary offsets (table files). Safe for concurrent use.
class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  virtual ~RandomAccessFile();

  // Reads up to n bytes starting at `offset`, possibly into `scratch` (at
  // least n bytes long). Returns fewer than n bytes only if the file ends.
  virtual Status Read(uint64_t offset, size_t n, std::string_view* result,
                      char* scratch) const = 0;
};

// A file written front to back. Appends are buffered in user space:
// Flush() hands them to the operating system, after which they survive a
// crash of this process; Sync() additionally forces them to stable storage,
// after which they survive power loss.
class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  virtual ~WritableFile();

  virtual Status Append(std::string_view data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

// An exclusive lock on a file; released when passed to Env::UnlockFile or
// destroyed.
class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  virtual ~FileLock();
};

// The file system interface used by the database. Every method is safe to
// call from multiple threads. Tests substitute implementations (usually an
// EnvWrapper) to inject failures.
class Env {
 public:
  Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  virtual ~Env();

  // The POSIX implementation. It lives for the whole process.
  static Env* Default();

  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;
  // Creates `fname`, truncating any existing file of that name.
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& fname) = 0;
  // Names (not paths) of the entries of `dir`, without "." and "..".
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;
  virtual Status RemoveFile(const std::string& fname) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;
  virtual Status RemoveDir(const std::string& dirname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  // Atomically replaces `target` (if it exists) with `src`.
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;
  // Makes the directory's entries durable: files created or renamed in it
  // survive power loss only after this succeeds.
  virtual Status SyncDir(const std::string& dirname) = 0;

  // Takes an exclusive lock on `fname`, creating the file if needed. Fails
  // with Busy if the lock is held, whether by another process or by another
  // holder in this process.
  virtual Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) = 0;
  virtual Status UnlockFile(std::unique_ptr<FileLock> lock) = 0;
};

// An Env that forwards every call to another Env. Subclasses override the
// operations they want to change.
class EnvWrapper : public Env {
 public:
  explicit EnvWrapper(Env* target) : target_(target) {}

  Env* target() const { return target_; }

  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    return target_->NewSequentialFile(fname, result);
  }
  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    return target_->NewRandomAccessFile(fname, result);
  }
  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    return target_->NewWritableFile(fname, result);
  }
  bool FileExists(const std::string& fname) override { return target_->FileExists(fname); }
  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    return target_->GetChildren(dir, result);
  }
  Status RemoveFile(const std::string& fname) override { return target_->RemoveFile(fname); }
  Status CreateDir(const std::string& dirname) override { return target_->CreateDir(dirname); }
  Status RemoveDir(const std::string& dirname) override { return target_->RemoveDir(dirname); }
  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    return target_->GetFileSize(fname, size);
  }
  Status RenameFile(const std::string& src, const std::string& target) override {
    return target_->RenameFile(src, target);
  }
  Status SyncDir(const std::string& dirname) override { return target_->SyncDir(dirname); }
  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    return target_->LockFile(fname, lock);
  }
  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    return target_->UnlockFile(std::move(lock));
  }

 private:
  Env* const target_;
};

}  // namespace kvlog
