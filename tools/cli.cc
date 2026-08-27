#include "cli.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string_view>

#include "kvlog/db.h"

namespace kvlog::cli {
namespace {

constexpr std::string_view kUsage =
    "usage: kvlog_cli [--sync] [--limit=N] <db-path> <command> [args]\n"
    "\n"
    "commands:\n"
    "  put <key> <value>        set key to value (creates the database if needed)\n"
    "  get <key>                print the value of key; exit code 1 if absent\n"
    "  delete <key>             remove key (creates the database if needed)\n"
    "  scan [<start> [<end>]]   print 'key<TAB>value' lines for keys in [start, end)\n"
    "  flush                    write buffered updates to a table file\n"
    "  stats                    print internal statistics\n"
    "\n"
    "options:\n"
    "  --sync      force the write-ahead log to disk before acknowledging writes\n"
    "  --limit=N   print at most N keys from scan\n";

struct Command {
  std::string_view name;
  size_t min_operands;
  size_t max_operands;
  bool writes;  // Write commands create a missing database; reads do not.
};

constexpr Command kCommands[] = {
    {"put", 2, 2, true},   {"get", 1, 1, false},  {"delete", 1, 1, true},
    {"scan", 0, 2, false}, {"flush", 0, 0, false}, {"stats", 0, 0, false},
};

const Command* FindCommand(std::string_view name) {
  for (const Command& command : kCommands) {
    if (command.name == name) {
      return &command;
    }
  }
  return nullptr;
}

int UsageError(std::ostream& err, std::string_view problem) {
  err << "kvlog_cli: " << problem << "\n\n" << kUsage;
  return kExitUsage;
}

int DatabaseError(std::ostream& err, const Status& s) {
  err << "kvlog_cli: " << s.ToString() << "\n";
  return kExitError;
}

}  // namespace

std::string Escape(const std::string& bytes) {
  std::string result;
  result.reserve(bytes.size());
  for (const char c : bytes) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte == '\\') {
      result.append("\\\\");
    } else if (byte >= 0x20 && byte < 0x7f) {
      result.push_back(c);
    } else {
      char escaped[5];
      std::snprintf(escaped, sizeof(escaped), "\\x%02x", byte);
      result.append(escaped);
    }
  }
  return result;
}

int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  bool sync = false;
  uint64_t limit = std::numeric_limits<uint64_t>::max();
  std::vector<std::string> positional;
  for (const std::string& arg : args) {
    if (arg == "--help" || arg == "-h") {
      out << kUsage;
      return kExitOk;
    }
    if (arg == "--sync") {
      sync = true;
    } else if (arg.starts_with("--limit=")) {
      const std::string_view number = std::string_view(arg).substr(8);
      const char* const end = number.data() + number.size();
      const auto [ptr, ec] = std::from_chars(number.data(), end, limit);
      if (number.empty() || ec != std::errc() || ptr != end) {
        return UsageError(err, "bad --limit value");
      }
    } else if (arg.starts_with("--")) {
      return UsageError(err, "unknown option " + arg);
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.size() < 2) {
    return UsageError(err, "missing database path or command");
  }
  const std::string& path = positional[0];
  const Command* const command = FindCommand(positional[1]);
  if (command == nullptr) {
    return UsageError(err, "unknown command " + positional[1]);
  }
  const std::vector<std::string> operands(positional.begin() + 2, positional.end());
  if (operands.size() < command->min_operands || operands.size() > command->max_operands) {
    return UsageError(err, "wrong number of arguments for " + positional[1]);
  }

  Options options;
  options.create_if_missing = command->writes;
  std::unique_ptr<DB> db;
  Status s = DB::Open(options, path, &db);
  if (!s.ok()) {
    return DatabaseError(err, s);
  }
  WriteOptions write_options;
  write_options.sync = sync;

  if (command->name == "put") {
    s = db->Put(write_options, operands[0], operands[1]);
  } else if (command->name == "delete") {
    s = db->Delete(write_options, operands[0]);
  } else if (command->name == "get") {
    std::string value;
    s = db->Get(ReadOptions(), operands[0], &value);
    if (s.IsNotFound()) {
      err << "kvlog_cli: key not found\n";
      return kExitNotFound;
    }
    if (s.ok()) {
      out << Escape(value) << "\n";
    }
  } else if (command->name == "scan") {
    const std::string start = operands.empty() ? std::string() : operands[0];
    const bool has_end = operands.size() == 2;
    std::unique_ptr<Iterator> iter = db->NewIterator(ReadOptions());
    uint64_t printed = 0;
    for (iter->Seek(start); iter->Valid() && printed < limit; iter->Next()) {
      if (has_end && iter->key() >= std::string_view(operands[1])) {
        break;
      }
      out << Escape(std::string(iter->key())) << "\t" << Escape(std::string(iter->value()))
          << "\n";
      ++printed;
    }
    s = iter->status();
  } else if (command->name == "flush") {
    s = db->Flush();
  } else if (command->name == "stats") {
    std::string stats;
    if (db->GetProperty("kvlog.stats", &stats)) {
      out << stats;
    }
  }
  if (!s.ok()) {
    return DatabaseError(err, s);
  }
  return kExitOk;
}

}  // namespace kvlog::cli
