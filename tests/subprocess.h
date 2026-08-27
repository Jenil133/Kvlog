#pragma once

#include <sys/types.h>

#include <cstdio>
#include <string>
#include <vector>

namespace kvlog::test {

// A child process whose stdin and stdout are pipes to the test. Used to run
// the crash helper, which the tests then kill or let finish.
class Subprocess {
 public:
  Subprocess() = default;
  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;
  // Kills (SIGKILL) and reaps the child if it is still running.
  ~Subprocess();

  // Starts the program at path argv[0] with arguments `argv`. Returns false
  // if it could not be started.
  bool Start(const std::vector<std::string>& argv);

  // Reads one line of the child's output, without the newline. Returns
  // false at the end of its output (it exited or closed stdout).
  bool ReadLine(std::string* line);

  // Gives the child end of file on its stdin.
  void CloseStdin();

  void Kill(int signal_number);

  // Waits for the child to exit and returns its waitpid() status.
  int Wait();

 private:
  pid_t pid_ = -1;
  int stdin_fd_ = -1;
  FILE* stdout_ = nullptr;
};

}  // namespace kvlog::test
