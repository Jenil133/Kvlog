#include "subprocess.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

extern char** environ;

namespace kvlog::test {

Subprocess::~Subprocess() {
  CloseStdin();
  if (pid_ > 0) {
    Kill(SIGKILL);
    Wait();
  }
  if (stdout_ != nullptr) {
    std::fclose(stdout_);
  }
}

bool Subprocess::Start(const std::vector<std::string>& argv) {
  int to_child[2];
  int from_child[2];
  if (::pipe(to_child) != 0) {
    return false;
  }
  if (::pipe(from_child) != 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    return false;
  }
  // The test's ends must not leak into this child or any other.
  ::fcntl(to_child[1], F_SETFD, FD_CLOEXEC);
  ::fcntl(from_child[0], F_SETFD, FD_CLOEXEC);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, to_child[0]);
  posix_spawn_file_actions_addclose(&actions, from_child[1]);

  std::vector<char*> args;
  for (const std::string& arg : argv) {
    args.push_back(const_cast<char*>(arg.c_str()));
  }
  args.push_back(nullptr);
  const int rc = ::posix_spawn(&pid_, args[0], &actions, nullptr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(to_child[0]);
  ::close(from_child[1]);
  if (rc != 0) {
    pid_ = -1;
    ::close(to_child[1]);
    ::close(from_child[0]);
    return false;
  }
  stdin_fd_ = to_child[1];
  stdout_ = ::fdopen(from_child[0], "r");
  return stdout_ != nullptr;
}

bool Subprocess::ReadLine(std::string* line) {
  line->clear();
  if (stdout_ == nullptr) {
    return false;
  }
  while (true) {
    const int c = std::fgetc(stdout_);
    if (c == EOF) {
      return !line->empty();
    }
    if (c == '\n') {
      return true;
    }
    line->push_back(static_cast<char>(c));
  }
}

void Subprocess::CloseStdin() {
  if (stdin_fd_ >= 0) {
    ::close(stdin_fd_);
    stdin_fd_ = -1;
  }
}

void Subprocess::Kill(int signal_number) {
  if (pid_ > 0) {
    ::kill(pid_, signal_number);
  }
}

int Subprocess::Wait() {
  int status = 0;
  if (pid_ <= 0) {
    return status;
  }
  while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
  }
  pid_ = -1;
  return status;
}

}  // namespace kvlog::test
