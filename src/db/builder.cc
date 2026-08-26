#include "db/builder.h"

#include <memory>

#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "table/table_builder.h"

namespace kvlog {

Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  const Comparator* comparator, TableCache* table_cache, Iterator* iter,
                  FileMetaData* meta) {
  meta->file_size = 0;
  iter->SeekToFirst();
  if (!iter->Valid()) {
    return iter->status();
  }

  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(TempFileName(dbname, meta->number), &file);
  if (!s.ok()) {
    return s;
  }

  uint64_t file_size = 0;
  {
    TableBuilder builder(options, comparator, file.get());
    meta->smallest.DecodeFrom(iter->key());
    for (; iter->Valid(); iter->Next()) {
      // Keep a copy: the iterator's key buffer is gone once it moves past
      // the last entry.
      meta->largest.DecodeFrom(iter->key());
      builder.Add(iter->key(), iter->value());
    }
    s = iter->status();
    if (s.ok()) {
      s = builder.Finish();
    } else {
      builder.Abandon();
    }
    file_size = builder.FileSize();
  }
  if (!s.ok()) {
    file.reset();
    DiscardTableFile(dbname, env, table_cache, meta->number);
    return s;
  }
  s = InstallTableFile(dbname, env, table_cache, meta->number, file_size, std::move(file));
  if (s.ok()) {
    meta->file_size = file_size;
  }
  return s;
}

Status InstallTableFile(const std::string& dbname, Env* env, TableCache* table_cache,
                        uint64_t number, uint64_t file_size, std::unique_ptr<WritableFile> file) {
  Status s = file->Sync();
  if (s.ok()) {
    s = file->Close();
  }
  file.reset();
  if (s.ok()) {
    s = env->RenameFile(TempFileName(dbname, number), TableFileName(dbname, number));
  }
  if (s.ok()) {
    s = env->SyncDir(dbname);
  }
  if (s.ok()) {
    // Reading the table back verifies the footer and index before anything
    // refers to the file, and leaves it open in the cache for the reads that
    // follow.
    std::unique_ptr<Iterator> check = table_cache->NewIterator(ReadOptions(), number, file_size);
    s = check->status();
  }
  if (!s.ok()) {
    DiscardTableFile(dbname, env, table_cache, number);
  }
  return s;
}

void DiscardTableFile(const std::string& dbname, Env* env, TableCache* table_cache,
                      uint64_t number) {
  table_cache->Evict(number);
  static_cast<void>(env->RemoveFile(TempFileName(dbname, number)));
  static_cast<void>(env->RemoveFile(TableFileName(dbname, number)));
}

}  // namespace kvlog
