#ifndef QEY_STATUS_H_
#define QEY_STATUS_H_

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qey {

enum class StatusCode {
  kOk,
  kNotInteger,       // A value could not be parsed as a 64-bit integer.
  kOutOfRange,       // An arithmetic result does not fit in a 64-bit integer.
  kInvalidArgument,  // The caller passed an unusable argument.
  kIoError,          // A file system or I/O operation failed.
  kCorruption,       // Stored data is malformed or fails its checksum.
  kUnavailable,      // A resource is held elsewhere, e.g. a locked file.
};

// The result of an operation that can fail. A default-constructed Status is
// OK.
class [[nodiscard]] Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string_view message)
      : code_(code), message_(message) {}

  static Status Ok() { return Status(); }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// Either a value of type T or a non-OK Status explaining why there is none.
template <typename T>
class [[nodiscard]] StatusOr {
 public:
  // Implicit, so functions can `return value;` or `return status;`.
  StatusOr(T value) : value_(std::move(value)) {}
  StatusOr(Status status) : status_(std::move(status)) {
    assert(!status_.ok());
  }

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }

  // Requires ok().
  const T& value() const {
    assert(ok());
    return *value_;
  }
  const T& operator*() const { return value(); }
  const T* operator->() const { return &value(); }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace qey

#endif  // QEY_STATUS_H_
