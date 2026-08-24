// POSIX implementation of Env. Builds on Linux and macOS; the differences
// that matter for durability are confined to SyncFd().

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "kvlog/env.h"

namespace kvlog {
namespace {

// Upper bound for a single read()/write() call; macOS rejects counts above
// INT_MAX with EINVAL.
constexpr size_t kMaxIoChunk = size_t{1} << 30;

Status PosixError(const std::string& context, int error_number) {
  const std::string message = std::generic_category().message(error_number);
  if (error_number == ENOENT) {
    return Status::NotFound(context, message);
  }
  return Status::IOError(context, message);
}

// Forces the data of `fd` (and, for a directory, its entries) to stable
// storage. Returns 0 on success, -1 with errno set on failure.
int SyncFd(int fd, [[maybe_unused]] bool is_directory) {
  int rc;
#if defined(__APPLE__)
  // On macOS fsync() only hands data to the drive, which may hold it in a
  // volatile cache; F_FULLFSYNC also flushes that cache. Some file systems do
  // not support F_FULLFSYNC, and for those fsync is the best available. Any
  // other failure (EIO, say) is a failed sync: retrying with the weaker
  // fsync and reporting success would claim durability that is not there.
  do {
    rc = ::fcntl(fd, F_FULLFSYNC);
  } while (rc != 0 && errno == EINTR);
  if (rc == 0) {
    return 0;
  }
  if (errno != ENOTSUP && errno != EOPNOTSUPP && errno != ENOTTY && errno != EINVAL) {
    return rc;
  }
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
#elif defined(__linux__)
  // fdatasync skips metadata that is not needed to read the data back (such
  // as mtime) but includes size changes, which is all a log or table needs.
  // Directory entries are metadata, so directories get a full fsync.
  do {
    rc = is_directory ? ::fsync(fd) : ::fdatasync(fd);
  } while (rc != 0 && errno == EINTR);
#else
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
#endif
  return rc;
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string filename, int fd) : filename_(std::move(filename)), fd_(fd) {}
  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    size_t total = 0;
    while (total < n) {
      const size_t chunk = std::min(n - total, kMaxIoChunk);
      const ssize_t r = ::read(fd_, scratch + total, chunk);
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        *result = std::string_view();
        return PosixError(filename_, errno);
      }
      if (r == 0) {
        break;  // End of file.
      }
      total += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, total);
    return Status::OK();
  }

 private:
  const std::string filename_;
  const int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string filename, int fd) : filename_(std::move(filename)), fd_(fd) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    size_t total = 0;
    while (total < n) {
      const size_t chunk = std::min(n - total, kMaxIoChunk);
      const ssize_t r = ::pread(fd_, scratch + total, chunk, static_cast<off_t>(offset + total));
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        *result = std::string_view();
        return PosixError(filename_, errno);
      }
      if (r == 0) {
        break;  // End of file.
      }
      total += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, total);
    return Status::OK();
  }

 private:
  const std::string filename_;
  const int fd_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string filename, int fd) : filename_(std::move(filename)), fd_(fd) {}

  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      // Callers that care about errors call Close() themselves.
      static_cast<void>(Close());
    }
  }

  Status Append(std::string_view data) override {
    if (fd_ < 0) {
      return Status::IOError(filename_, "append to a closed file");
    }
    // Fill the buffer first; small appends (the common case for log records)
    // then cost a memcpy instead of a system call.
    const size_t copy = std::min(data.size(), kBufferSize - pos_);
    if (copy > 0) {
      std::memcpy(buf_ + pos_, data.data(), copy);
      pos_ += copy;
      data.remove_prefix(copy);
    }
    if (data.empty()) {
      return Status::OK();
    }
    Status s = FlushBuffer();
    if (!s.ok()) {
      return s;
    }
    if (data.size() < kBufferSize) {
      std::memcpy(buf_, data.data(), data.size());
      pos_ = data.size();
      return Status::OK();
    }
    return WriteUnbuffered(data.data(), data.size());
  }

  Status Flush() override {
    if (fd_ < 0) {
      return Status::IOError(filename_, "flush of a closed file");
    }
    return FlushBuffer();
  }

  Status Sync() override {
    if (fd_ < 0) {
      return Status::IOError(filename_, "sync of a closed file");
    }
    Status s = FlushBuffer();
    if (!s.ok()) {
      return s;
    }
    if (SyncFd(fd_, /*is_directory=*/false) != 0) {
      return PosixError("sync " + filename_, errno);
    }
    return Status::OK();
  }

  Status Close() override {
    if (fd_ < 0) {
      return Status::OK();
    }
    Status s = FlushBuffer();
    if (::close(fd_) != 0 && s.ok()) {
      s = PosixError("close " + filename_, errno);
    }
    fd_ = -1;
    return s;
  }

 private:
  static constexpr size_t kBufferSize = 64 * 1024;

  Status FlushBuffer() {
    Status s = WriteUnbuffered(buf_, pos_);
    pos_ = 0;
    return s;
  }

  Status WriteUnbuffered(const char* data, size_t size) {
    while (size > 0) {
      const ssize_t written = ::write(fd_, data, std::min(size, kMaxIoChunk));
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(filename_, errno);
      }
      data += written;
      size -= static_cast<size_t>(written);
    }
    return Status::OK();
  }

  const std::string filename_;
  int fd_;
  size_t pos_ = 0;
  char buf_[kBufferSize];
};

// Names locked through this Env. flock() already excludes other processes,
// but whether two opens within one process conflict depends on the file
// system, so in-process exclusion is tracked explicitly as well.
class LockTable {
 public:
  bool Insert(const std::string& fname) {
    std::lock_guard<std::mutex> guard(mu_);
    return names_.insert(fname).second;
  }
  void Remove(const std::string& fname) {
    std::lock_guard<std::mutex> guard(mu_);
    names_.erase(fname);
  }

 private:
  std::mutex mu_;
  std::set<std::string> names_;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(int fd, std::string fname, LockTable* table)
      : fd_(fd), fname_(std::move(fname)), table_(table) {}

  ~PosixFileLock() override {
    if (fd_ >= 0) {
      static_cast<void>(Release());
    }
  }

  Status Release() {
    Status s;
    if (::flock(fd_, LOCK_UN) != 0) {
      s = PosixError("unlock " + fname_, errno);
    }
    // Closing the descriptor drops the lock even if LOCK_UN failed.
    ::close(fd_);
    fd_ = -1;
    table_->Remove(fname_);
    return s;
  }

 private:
  int fd_;
  const std::string fname_;
  LockTable* const table_;
};

class PosixEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    result->reset();
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return PosixError(fname, errno);
    }
    *result = std::make_unique<PosixSequentialFile>(fname, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
    result->reset();
    const int fd = ::open(fname.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return PosixError(fname, errno);
    }
    *result = std::make_unique<PosixRandomAccessFile>(fname, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    result->reset();
    const int fd = ::open(fname.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      return PosixError(fname, errno);
    }
    *result = std::make_unique<PosixWritableFile>(fname, fd);
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override { return ::access(fname.c_str(), F_OK) == 0; }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
      return PosixError(dir, errno);
    }
    while (true) {
      // readdir() returns nullptr both at the end and on an error; only
      // errno tells them apart. A silently truncated listing would make
      // recovery skip logs and reuse their numbers.
      errno = 0;
      const struct dirent* entry = ::readdir(d);
      if (entry == nullptr) {
        const int error_number = errno;
        ::closedir(d);
        if (error_number != 0) {
          result->clear();
          return PosixError(dir, error_number);
        }
        return Status::OK();
      }
      const std::string_view name(entry->d_name);
      if (name != "." && name != "..") {
        result->emplace_back(name);
      }
    }
  }

  Status RemoveFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) {
      return PosixError(fname, errno);
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0) {
      return PosixError(dirname, errno);
    }
    return Status::OK();
  }

  Status RemoveDir(const std::string& dirname) override {
    if (::rmdir(dirname.c_str()) != 0) {
      return PosixError(dirname, errno);
    }
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat info;
    if (::stat(fname.c_str(), &info) != 0) {
      *size = 0;
      return PosixError(fname, errno);
    }
    *size = static_cast<uint64_t>(info.st_size);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    if (::rename(src.c_str(), target.c_str()) != 0) {
      return PosixError("rename " + src + " to " + target, errno);
    }
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    int flags = O_RDONLY | O_CLOEXEC;
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(dirname.c_str(), flags);
    if (fd < 0) {
      return PosixError(dirname, errno);
    }
    Status s;
    if (SyncFd(fd, /*is_directory=*/true) != 0) {
      s = PosixError("sync directory " + dirname, errno);
    }
    ::close(fd);
    return s;
  }

  Status LockFile(const std::string& fname, std::unique_ptr<FileLock>* lock) override {
    lock->reset();
    if (!locks_.Insert(fname)) {
      return Status::Busy("lock " + fname, "already held by this process");
    }
    const int fd = ::open(fname.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
      const int error_number = errno;
      locks_.Remove(fname);
      return PosixError("lock " + fname, error_number);
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int error_number = errno;
      ::close(fd);
      locks_.Remove(fname);
      if (error_number == EWOULDBLOCK) {
        return Status::Busy("lock " + fname, "held by another process");
      }
      return PosixError("lock " + fname, error_number);
    }
    *lock = std::make_unique<PosixFileLock>(fd, fname, &locks_);
    return Status::OK();
  }

  Status UnlockFile(std::unique_ptr<FileLock> lock) override {
    if (lock == nullptr) {
      return Status::InvalidArgument("unlock of a null lock");
    }
    return static_cast<PosixFileLock*>(lock.get())->Release();
  }

 private:
  LockTable locks_;
};

}  // namespace

Env* Env::Default() {
  // Intentionally never destroyed: background threads of databases that are
  // still open at exit may use it during static destruction.
  static Env* const env = new PosixEnv();
  return env;
}

}  // namespace kvlog
