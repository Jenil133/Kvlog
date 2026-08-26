#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kvlog/env.h"
#include "kvlog/iterator.h"
#include "kvlog/options.h"
#include "kvlog/status.h"
#include "util/comparator.h"

namespace kvlog {

struct FileMetaData;
class TableCache;

// Writes every entry of `iter` (internal keys in order) to table file
// `meta->number` and fills in the rest of *meta.
//
// The table never becomes visible half-written. It is built under a
// temporary name, synced, renamed to its final name, and the directory is
// synced; then it is opened through `table_cache` to check that it reads
// back. Even then it is not part of the database until the caller records it
// in the MANIFEST: until that happens, recovery treats it as an orphan and
// deletes it.
//
// If `iter` is empty no file is created and meta->file_size is 0. On error
// any partial file is removed.
Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  const Comparator* comparator, TableCache* table_cache, Iterator* iter,
                  FileMetaData* meta);

// The second half of the protocol above, for callers that write the table
// themselves (compaction): `file` holds a finished table of `file_size` bytes
// written under TempFileName(dbname, number). Syncs and closes it, renames it
// to TableFileName(dbname, number), syncs the directory and checks that the
// table opens through `table_cache`. On error removes both names.
Status InstallTableFile(const std::string& dbname, Env* env, TableCache* table_cache,
                        uint64_t number, uint64_t file_size, std::unique_ptr<WritableFile> file);

// Removes table `number` under both its temporary and its final name, for a
// table whose writing failed or was abandoned.
void DiscardTableFile(const std::string& dbname, Env* env, TableCache* table_cache,
                      uint64_t number);

}  // namespace kvlog
