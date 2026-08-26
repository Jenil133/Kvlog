#pragma once

// Names of the files in a database directory. Every numbered file draws its
// number from one counter (VersionSet::NewFileNumber), so numbers are unique
// across kinds and increase over time.
//
//   CURRENT            name of the live MANIFEST, replaced atomically
//   LOCK               flock()ed while the database is open
//   MANIFEST-<number>  log of version edits (the set of live table files)
//   <number>.log       write-ahead log
//   <number>.sst       table file
//   <number>.tmp       file being written; renamed into place when complete

#include <cstdint>
#include <string>
#include <string_view>

#include "kvlog/env.h"
#include "kvlog/status.h"

namespace kvlog {

enum class FileType {
  kLogFile,
  kLockFile,
  kTableFile,
  kDescriptorFile,  // MANIFEST
  kCurrentFile,
  kTempFile,
};

// Subdirectory where recovery puts logs it could not fully replay (see
// DBImpl::QuarantineSkippedLogs). ParseFileName does not accept the name, so
// nothing else in the database touches it.
inline constexpr char kLostDirName[] = "lost";

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string TempFileName(const std::string& dbname, uint64_t number);
std::string DescriptorFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);

// Parses a file name (not a path). Returns false for names the database did
// not create; those files are never touched.
bool ParseFileName(std::string_view filename, uint64_t* number, FileType* type);

// Atomically points CURRENT at MANIFEST-<descriptor_number>: the new content
// is written to a temporary file, synced, renamed over CURRENT, and the
// directory synced, so after a crash CURRENT names either the old or the new
// MANIFEST, never a mix. *rename_attempted (if given) is set once CURRENT
// may have changed: from then on, even on failure, CURRENT may name the new
// MANIFEST.
Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number,
                      bool* rename_attempted = nullptr);

}  // namespace kvlog
