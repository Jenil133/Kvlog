// Command-line access to a kvlog database: put, get, delete, scan, flush and
// stats. Run with --help for usage.

#include <iostream>
#include <string>
#include <vector>

#include "cli.h"

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  return kvlog::cli::Run(args, std::cout, std::cerr);
}
