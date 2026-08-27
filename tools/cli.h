#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace kvlog::cli {

// Exit codes of the command-line tool.
inline constexpr int kExitOk = 0;
inline constexpr int kExitNotFound = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitError = 3;

// Runs the kvlog command-line tool with `args` (the arguments after the
// program name), printing results to `out` and diagnostics to `err`.
// Returns the process exit code.
int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

// Renders arbitrary bytes for display: printable ASCII as-is, backslash as
// "\\", everything else as "\xHH".
std::string Escape(const std::string& bytes);

}  // namespace kvlog::cli
