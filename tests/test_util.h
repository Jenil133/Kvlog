#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kvlog/db.h"
#include "kvlog/env.h"
#include "kvlog/status.h"

namespace kvlog::test {

// Passes for an OK status; otherwise fails showing the status text.
inline testing::AssertionResult IsOk(const Status& s) {
  if (s.ok()) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << s.ToString();
}

}  // namespace kvlog::test

// Like ASSERT_TRUE/EXPECT_TRUE, these accept extra context with <<.
#define ASSERT_OK(expr) ASSERT_TRUE(::kvlog::test::IsOk(expr))
#define EXPECT_OK(expr) EXPECT_TRUE(::kvlog::test::IsOk(expr))

namespace kvlog::test {

// A new, empty directory under the system temporary directory, removed
// together with its contents when the object is destroyed.
class TempDir {
 public:
  TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir();

  const std::string& path() const { return path_; }
  // path()/name; not created.
  std::string Sub(std::string_view name) const;

 private:
  std::string path_;
};

// `length` arbitrary bytes, including 0x00 and 0xff.
std::string RandomBytes(std::mt19937_64* rng, size_t length);

// Printable keys with a fixed-width number, so string order equals numeric
// order: Key(7) == "key000007".
std::string Key(uint64_t i);

std::string ReadFileBytes(const std::string& path);
void WriteFileBytes(const std::string& path, const std::string& contents);
uint64_t FileSize(const std::string& path);
void TruncateFile(const std::string& path, uint64_t size);
// XORs the byte at `offset` with `mask`.
void CorruptByte(const std::string& path, uint64_t offset, unsigned char mask = 0x55);
void RemoveFile(const std::string& path);
// Removes a directory and everything in it.
void RemoveAll(const std::string& path);

// Sorted names of the entries of `dir`.
std::vector<std::string> ListDir(const std::string& dir);
// Sorted full paths of the files in `dir` whose names end with `suffix`.
std::vector<std::string> FilesWithSuffix(const std::string& dir, std::string_view suffix);
// Copies the regular files of `from` into the new directory `to`.
void CopyDir(const std::string& from, const std::string& to);

// Reads every key/value of `db` through an iterator, checking that keys come
// out strictly increasing and that the iterator ends without error.
std::map<std::string, std::string> ScanAll(DB* db);

// Returns the value of `key`, "NOT_FOUND", or the error text.
std::string GetOrStatus(DB* db, std::string_view key);

// In-memory files, for testing the log and table formats without a file
// system. Each refers to a std::string owned by the test, which must outlive
// the file object.

// Appends to *contents. Close() and later appends fail, like a real file.
class StringSink final : public WritableFile {
 public:
  explicit StringSink(std::string* contents) : contents_(contents) {}

  Status Append(std::string_view data) override;
  Status Flush() override;
  Status Sync() override;
  Status Close() override;

  // Makes every following Append fail with an I/O error.
  void FailAppends() { fail_appends_ = true; }

 private:
  std::string* const contents_;
  bool closed_ = false;
  bool fail_appends_ = false;
};

// Reads *contents front to back. After FailAfter(n), any read reaching past
// the first n bytes fails with an I/O error.
class StringSource final : public SequentialFile {
 public:
  explicit StringSource(const std::string* contents) : contents_(contents) {}

  Status Read(size_t n, std::string_view* result, char* scratch) override;

  void FailAfter(size_t offset) { fail_after_ = offset; }

 private:
  const std::string* const contents_;
  size_t pos_ = 0;
  size_t fail_after_ = std::string::npos;
};

// Random access to *contents. Counts reads so tests can check how many
// blocks a lookup touched.
class StringFile final : public RandomAccessFile {
 public:
  explicit StringFile(const std::string* contents) : contents_(contents) {}

  Status Read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override;

  int reads() const { return reads_.load(); }

 private:
  const std::string* const contents_;
  // Atomic because RandomAccessFile promises concurrent use.
  mutable std::atomic<int> reads_{0};
};

// The default Env with Sync() and SyncDir() reduced to handing data to the
// operating system. That is all a process crash requires, so recovery
// results are unchanged; only power-loss durability is given up. Tests that
// flush many times use it because a real sync (F_FULLFSYNC on macOS) can
// take hundreds of milliseconds on a busy machine.
class NoSyncEnv final : public EnvWrapper {
 public:
  NoSyncEnv() : EnvWrapper(Env::Default()) {}

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override;
  Status SyncDir(const std::string&) override { return Status::OK(); }
};

}  // namespace kvlog::test
