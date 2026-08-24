#include "kvlog/status.h"

namespace kvlog {

Status::Status(Code code, std::string_view msg, std::string_view detail) : code_(code) {
  message_.reserve(msg.size() + (detail.empty() ? 0 : detail.size() + 2));
  message_.append(msg);
  if (!detail.empty()) {
    message_.append(": ");
    message_.append(detail);
  }
}

std::string Status::ToString() const {
  const char* name = "OK";
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      name = "NotFound";
      break;
    case Code::kCorruption:
      name = "Corruption";
      break;
    case Code::kIOError:
      name = "IO error";
      break;
    case Code::kInvalidArgument:
      name = "Invalid argument";
      break;
    case Code::kBusy:
      name = "Busy";
      break;
  }
  std::string result(name);
  if (!message_.empty()) {
    result.append(": ");
    result.append(message_);
  }
  return result;
}

}  // namespace kvlog
