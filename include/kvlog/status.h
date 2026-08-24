#pragma once

#include <string>
#include <string_view>

namespace kvlog {

// The outcome of an operation. A default-constructed Status is OK; an error
// Status carries a code for callers to branch on and a message for humans.
//
// Status is [[nodiscard]], so ignoring one is a compiler warning: in a storage
// engine a dropped I/O error usually means silently lost data. Call sites that
// really do not care cast the result to void and say why.
class [[nodiscard]] Status {
 public:
  enum class Code : unsigned char {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kIOError = 3,
    kInvalidArgument = 4,
    kBusy = 5,
  };

  Status() noexcept = default;

  static Status OK() noexcept { return Status(); }
  static Status NotFound(std::string_view msg, std::string_view detail = {}) {
    return Status(Code::kNotFound, msg, detail);
  }
  static Status Corruption(std::string_view msg, std::string_view detail = {}) {
    return Status(Code::kCorruption, msg, detail);
  }
  static Status IOError(std::string_view msg, std::string_view detail = {}) {
    return Status(Code::kIOError, msg, detail);
  }
  static Status InvalidArgument(std::string_view msg, std::string_view detail = {}) {
    return Status(Code::kInvalidArgument, msg, detail);
  }
  static Status Busy(std::string_view msg, std::string_view detail = {}) {
    return Status(Code::kBusy, msg, detail);
  }

  bool ok() const noexcept { return code_ == Code::kOk; }
  bool IsNotFound() const noexcept { return code_ == Code::kNotFound; }
  bool IsCorruption() const noexcept { return code_ == Code::kCorruption; }
  bool IsIOError() const noexcept { return code_ == Code::kIOError; }
  bool IsInvalidArgument() const noexcept { return code_ == Code::kInvalidArgument; }
  bool IsBusy() const noexcept { return code_ == Code::kBusy; }

  Code code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

  // "OK", or the code name followed by the message, e.g. "IO error: x: y".
  std::string ToString() const;

 private:
  Status(Code code, std::string_view msg, std::string_view detail);

  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace kvlog
