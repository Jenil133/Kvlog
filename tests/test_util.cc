#include "test_util.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>

namespace kvlog::test {

namespace fs = std::filesystem;

TempDir::TempDir() {
  std::string pattern = (fs::temp_directory_path() / "kvlog_test_XXXXXX").string();
  char* created = ::mkdtemp(pattern.data());
  if (created == nullptr) {
    ADD_FAILURE() << "mkdtemp failed for " << pattern;
    std::abort();
  }
  path_ = created;
}

TempDir::~TempDir() {
  std::error_code ignored;
  fs::remove_all(path_, ignored);
}

std::string TempDir::Sub(std::string_view name) const {
  return path_ + "/" + std::string(name);
}

std::string RandomBytes(std::mt19937_64* rng, size_t length) {
  std::string result(length, '\0');
  std::uniform_int_distribution<int> byte(0, 255);
  for (char& c : result) {
    c = static_cast<char>(byte(*rng));
  }
  return result;
}

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%06llu", static_cast<unsigned long long>(i));
  return buf;
}

std::string ReadFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "cannot open " << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::string& path, const std::string& contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  EXPECT_TRUE(out.good()) << "cannot write " << path;
}

uint64_t FileSize(const std::string& path) { return fs::file_size(path); }

void TruncateFile(const std::string& path, uint64_t size) { fs::resize_file(path, size); }

void CorruptByte(const std::string& path, uint64_t offset, unsigned char mask) {
  std::string contents = ReadFileBytes(path);
  ASSERT_LT(offset, contents.size());
  contents[offset] = static_cast<char>(static_cast<unsigned char>(contents[offset]) ^ mask);
  WriteFileBytes(path, contents);
}

void RemoveFile(const std::string& path) {
  EXPECT_TRUE(fs::remove(path)) << "cannot remove " << path;
}

void RemoveAll(const std::string& path) {
  std::error_code error;
  fs::remove_all(path, error);
  EXPECT_FALSE(error) << "cannot remove " << path << ": " << error.message();
}

std::vector<std::string> ListDir(const std::string& dir) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> FilesWithSuffix(const std::string& dir, std::string_view suffix) {
  std::vector<std::string> paths;
  for (const std::string& name : ListDir(dir)) {
    if (name.ends_with(suffix)) {
      paths.push_back(dir + "/" + name);
    }
  }
  return paths;
}

void CopyDir(const std::string& from, const std::string& to) {
  fs::create_directory(to);
  for (const auto& entry : fs::directory_iterator(from)) {
    if (entry.is_regular_file()) {
      fs::copy_file(entry.path(), fs::path(to) / entry.path().filename());
    }
  }
}

std::map<std::string, std::string> ScanAll(DB* db) {
  std::map<std::string, std::string> result;
  std::unique_ptr<Iterator> iter = db->NewIterator(ReadOptions());
  std::string previous;
  bool first = true;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    const std::string key(iter->key());
    if (!first) {
      EXPECT_LT(previous, key) << "iterator keys out of order";
    }
    first = false;
    previous = key;
    result.emplace(key, std::string(iter->value()));
  }
  EXPECT_TRUE(iter->status().ok()) << iter->status().ToString();
  return result;
}

std::string GetOrStatus(DB* db, std::string_view key) {
  std::string value;
  const Status s = db->Get(ReadOptions(), key, &value);
  if (s.IsNotFound()) {
    return "NOT_FOUND";
  }
  if (!s.ok()) {
    return s.ToString();
  }
  return value;
}

Status StringSink::Append(std::string_view data) {
  if (closed_) {
    return Status::IOError("append to a closed file");
  }
  if (fail_appends_) {
    return Status::IOError("injected append failure");
  }
  contents_->append(data.data(), data.size());
  return Status::OK();
}

Status StringSink::Flush() {
  return closed_ ? Status::IOError("flush of a closed file") : Status();
}

Status StringSink::Sync() {
  return closed_ ? Status::IOError("sync of a closed file") : Status();
}

Status StringSink::Close() {
  closed_ = true;
  return Status::OK();
}

Status StringSource::Read(size_t n, std::string_view* result, char* scratch) {
  const size_t available = contents_->size() - pos_;
  const size_t count = std::min(n, available);
  if (pos_ + count > fail_after_) {
    *result = std::string_view();
    return Status::IOError("injected read failure");
  }
  std::copy_n(contents_->data() + pos_, count, scratch);
  pos_ += count;
  *result = std::string_view(scratch, count);
  return Status::OK();
}

Status StringFile::Read(uint64_t offset, size_t n, std::string_view* result,
                        char* scratch) const {
  reads_.fetch_add(1);
  if (offset > contents_->size()) {
    *result = std::string_view();
    return Status::IOError("read past the end of the file");
  }
  const size_t count = std::min<uint64_t>(n, contents_->size() - offset);
  std::copy_n(contents_->data() + offset, count, scratch);
  *result = std::string_view(scratch, count);
  return Status::OK();
}

namespace {

class NoSyncFile final : public WritableFile {
 public:
  explicit NoSyncFile(std::unique_ptr<WritableFile> base) : base_(std::move(base)) {}

  Status Append(std::string_view data) override { return base_->Append(data); }
  Status Flush() override { return base_->Flush(); }
  Status Sync() override { return base_->Flush(); }
  Status Close() override { return base_->Close(); }

 private:
  const std::unique_ptr<WritableFile> base_;
};

}  // namespace

Status NoSyncEnv::NewWritableFile(const std::string& fname,
                                  std::unique_ptr<WritableFile>* result) {
  std::unique_ptr<WritableFile> file;
  Status s = target()->NewWritableFile(fname, &file);
  if (s.ok()) {
    *result = std::make_unique<NoSyncFile>(std::move(file));
  }
  return s;
}

}  // namespace kvlog::test
