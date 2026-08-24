#include "kvlog/env.h"

#include <memory>

#include "util/env_util.h"

namespace kvlog {

SequentialFile::~SequentialFile() = default;
RandomAccessFile::~RandomAccessFile() = default;
WritableFile::~WritableFile() = default;
FileLock::~FileLock() = default;
Env::~Env() = default;

Status WriteStringToFile(Env* env, std::string_view data, const std::string& fname, bool sync) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  s = file->Append(data);
  if (s.ok() && sync) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  file.reset();
  if (!s.ok()) {
    // Best effort: the caller already gets the original error.
    static_cast<void>(env->RemoveFile(fname));
  }
  return s;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(fname, &file);
  if (!s.ok()) {
    return s;
  }
  constexpr size_t kChunk = 8192;
  auto scratch = std::make_unique_for_overwrite<char[]>(kChunk);
  while (true) {
    std::string_view fragment;
    s = file->Read(kChunk, &fragment, scratch.get());
    if (!s.ok()) {
      break;
    }
    data->append(fragment.data(), fragment.size());
    if (fragment.size() < kChunk) {
      break;
    }
  }
  return s;
}

Status CopyFileSynced(Env* env, const std::string& src, const std::string& dst) {
  std::unique_ptr<SequentialFile> in;
  Status s = env->NewSequentialFile(src, &in);
  if (!s.ok()) {
    return s;
  }
  std::unique_ptr<WritableFile> out;
  s = env->NewWritableFile(dst, &out);
  if (!s.ok()) {
    return s;
  }
  constexpr size_t kChunk = 64 * 1024;
  auto scratch = std::make_unique_for_overwrite<char[]>(kChunk);
  while (s.ok()) {
    std::string_view fragment;
    s = in->Read(kChunk, &fragment, scratch.get());
    if (s.ok() && !fragment.empty()) {
      s = out->Append(fragment);
    }
    if (fragment.size() < kChunk) {
      break;
    }
  }
  if (s.ok()) {
    s = out->Sync();
  }
  const Status close_status = out->Close();
  if (s.ok()) {
    s = close_status;
  }
  out.reset();
  if (!s.ok()) {
    static_cast<void>(env->RemoveFile(dst));
  }
  return s;
}

}  // namespace kvlog
