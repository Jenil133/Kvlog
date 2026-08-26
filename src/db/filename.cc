#include "db/filename.h"

#include <cstdio>
#include <limits>

#include "util/env_util.h"

namespace kvlog {
namespace {

std::string NumberedName(uint64_t number, const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return buf;
}

// Parses a non-empty run of decimal digits that makes up all of `digits`.
bool ParseNumber(std::string_view digits, uint64_t* number) {
  if (digits.empty()) {
    return false;
  }
  uint64_t value = 0;
  for (const char c : digits) {
    if (c < '0' || c > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;  // Overflow.
    }
    value = value * 10 + digit;
  }
  *number = value;
  return true;
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return dbname + "/" + NumberedName(number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return dbname + "/" + NumberedName(number, "sst");
}

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return dbname + "/" + NumberedName(number, "tmp");
}

std::string DescriptorFileName(const std::string& dbname, uint64_t number) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "MANIFEST-%06llu", static_cast<unsigned long long>(number));
  return dbname + "/" + buf;
}

std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }

bool ParseFileName(std::string_view filename, uint64_t* number, FileType* type) {
  if (filename == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrentFile;
    return true;
  }
  if (filename == "LOCK") {
    *number = 0;
    *type = FileType::kLockFile;
    return true;
  }
  constexpr std::string_view kManifestPrefix = "MANIFEST-";
  if (filename.starts_with(kManifestPrefix)) {
    if (!ParseNumber(filename.substr(kManifestPrefix.size()), number)) {
      return false;
    }
    *type = FileType::kDescriptorFile;
    return true;
  }
  const size_t dot = filename.find('.');
  if (dot == std::string_view::npos || !ParseNumber(filename.substr(0, dot), number)) {
    return false;
  }
  const std::string_view suffix = filename.substr(dot + 1);
  if (suffix == "log") {
    *type = FileType::kLogFile;
  } else if (suffix == "sst") {
    *type = FileType::kTableFile;
  } else if (suffix == "tmp") {
    *type = FileType::kTempFile;
  } else {
    return false;
  }
  return true;
}

Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t descriptor_number,
                      bool* rename_attempted) {
  std::string contents = DescriptorFileName(dbname, descriptor_number);
  contents.erase(0, dbname.size() + 1);  // Store the name, not the path.
  contents.push_back('\n');
  const std::string temp = TempFileName(dbname, descriptor_number);
  Status s = WriteStringToFile(env, contents, temp, /*sync=*/true);
  if (s.ok()) {
    if (rename_attempted != nullptr) {
      *rename_attempted = true;
    }
    s = env->RenameFile(temp, CurrentFileName(dbname));
    if (!s.ok()) {
      static_cast<void>(env->RemoveFile(temp));
    }
  }
  if (s.ok()) {
    s = env->SyncDir(dbname);
  }
  return s;
}

}  // namespace kvlog
