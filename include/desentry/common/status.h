#pragma once
// Lightweight Result/Status type used everywhere instead of exceptions for
// expected-failure control flow (disk errors, malformed input, not-found).
// Exceptions are still used for genuine programmer-error invariants
// (asserts), mirroring how production C++ codebases split the two.

#include <optional>
#include <string>
#include <utility>

namespace desentry {

enum class StatusCode {
  kOk = 0,
  kNotFound,
  kAlreadyExists,
  kInvalidArgument,
  kIOError,
  kCorruption,
  kOutOfSpace,
  kSchemaViolation,
  kAuthError,
  kNetworkError,
  kInternal,
};

class Status {
 public:
  Status() : code_(StatusCode::kOk) {}
  Status(StatusCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg) { return Status(StatusCode::kNotFound, std::move(msg)); }
  static Status AlreadyExists(std::string msg) { return Status(StatusCode::kAlreadyExists, std::move(msg)); }
  static Status InvalidArgument(std::string msg) { return Status(StatusCode::kInvalidArgument, std::move(msg)); }
  static Status IOError(std::string msg) { return Status(StatusCode::kIOError, std::move(msg)); }
  static Status Corruption(std::string msg) { return Status(StatusCode::kCorruption, std::move(msg)); }
  static Status OutOfSpace(std::string msg) { return Status(StatusCode::kOutOfSpace, std::move(msg)); }
  static Status SchemaViolation(std::string msg) { return Status(StatusCode::kSchemaViolation, std::move(msg)); }
  static Status AuthError(std::string msg) { return Status(StatusCode::kAuthError, std::move(msg)); }
  static Status NetworkError(std::string msg) { return Status(StatusCode::kNetworkError, std::move(msg)); }
  static Status Internal(std::string msg) { return Status(StatusCode::kInternal, std::move(msg)); }

  bool ok() const { return code_ == StatusCode::kOk; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }

  std::string ToString() const {
    if (ok()) return "OK";
    return message_;
  }

 private:
  StatusCode code_;
  std::string message_;
};

// A value-or-status result type. Deliberately minimal (no monadic sugar) --
// this is a systems codebase, not a functional-programming exercise.
template <typename T>
class StatusOr {
 public:
  StatusOr(Status status) : status_(std::move(status)) {}  // NOLINT
  StatusOr(T value) : status_(Status::OK()), value_(std::move(value)) {}  // NOLINT

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }

  T& value() { return *value_; }
  const T& value() const { return *value_; }
  T ValueOrDie() {
    if (!ok()) {
      // Programmer error: caller didn't check ok() first.
      std::terminate();
    }
    return std::move(*value_);
  }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace desentry
