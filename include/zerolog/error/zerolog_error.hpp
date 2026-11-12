/** @file @brief Recoverable logging errors and cold standard-library diagnostics. */
#pragma once
#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <uring/error/uring_error.hpp>
namespace zerolog {
enum class ErrorCode : std::uint8_t {
  kSuccess = 0,           /**< Operation completed. */
  kWouldBlock,            /**< Retry after pending output makes progress. */
  kConsumerBusy,          /**< Another consumer step is active. */
  kLeaseHeld,             /**< Exclusive consumer ownership is already claimed. */
  kLeasePoisoned,         /**< Consumer generation cannot be reused. */
  kQueueFull,             /**< Producer ring has insufficient credit. */
  kRecordTooLarge,        /**< One record exceeds the maximum representable extent. */
  kTooManyThreads,        /**< Every registry slot is occupied. */
  kBudgetExceeded,        /**< A bounded step consumed its work allowance. */
  kWriteFailed,           /**< Target I/O failed without making the engine unavailable. */
  kShortWrite,            /**< Only part of an output span was written. */
  kRotateFailed,          /**< A file rotation step failed. */
  kProactorUnavailable,   /**< The host cannot provide the configured proactor. */
  kShuttingDown,          /**< The lifecycle no longer accepts new work. */
  kMembarrierUnavailable, /**< The private expedited barrier is unavailable. */
  kInvalidArgument,       /**< A supplied configuration or value is invalid. */
};
/** @brief Cold std::error_category adapter; the hot error carrier is ErrorCode. */
class ErrorCategory final : public std::error_category {
 public:
  /** @brief Stable diagnostic domain name. */
  [[nodiscard]] const char* name() const noexcept override { return "zerolog"; }
  /** @brief Human-readable message; unknown numeric codes are not narrowed. */
  [[nodiscard]] std::string message(int value) const override {
    if (value < 0 || value > static_cast<int>(ErrorCode::kInvalidArgument)) return "unknown zerolog error";
    switch (static_cast<ErrorCode>(value)) {
      case ErrorCode::kSuccess:
        return "success";
      case ErrorCode::kWouldBlock:
        return "would block";
      case ErrorCode::kConsumerBusy:
        return "consumer busy";
      case ErrorCode::kLeaseHeld:
        return "lease held";
      case ErrorCode::kLeasePoisoned:
        return "lease poisoned";
      case ErrorCode::kQueueFull:
        return "queue full";
      case ErrorCode::kRecordTooLarge:
        return "record too large";
      case ErrorCode::kTooManyThreads:
        return "too many threads";
      case ErrorCode::kBudgetExceeded:
        return "budget exceeded";
      case ErrorCode::kWriteFailed:
        return "write failed";
      case ErrorCode::kShortWrite:
        return "short write";
      case ErrorCode::kRotateFailed:
        return "rotate failed";
      case ErrorCode::kProactorUnavailable:
        return "proactor unavailable";
      case ErrorCode::kShuttingDown:
        return "shutting down";
      case ErrorCode::kMembarrierUnavailable:
        return "membarrier unavailable";
      case ErrorCode::kInvalidArgument:
        return "invalid argument";
    }
    return "unknown zerolog error";
  }
};
/** @brief Process-lifetime category singleton. */
[[nodiscard]] inline const std::error_category& Category() noexcept {
  static ErrorCategory category;
  return category;
}
/** @brief Preserve a domain code in the standard diagnostic carrier. */
[[nodiscard]] inline std::error_code make_error_code(ErrorCode code) noexcept { return {static_cast<int>(code), Category()}; }
/** @brief Recoverable result without exceptions. */
template <class T = void>
using Result = std::expected<T, ErrorCode>;
/** @brief Separate unavailable-engine failures from retryable target I/O failures. */
[[nodiscard]] constexpr ErrorCode FromUring(uring::UringErrorCode code) noexcept {
  if (code == uring::UringErrorCode::kAgain || code == uring::UringErrorCode::kSqRingFull ||
      code == uring::UringErrorCode::kSlabExhausted)
    return ErrorCode::kWouldBlock;
  if (code == uring::UringErrorCode::kBusy || code == uring::UringErrorCode::kReentrantDrive) return ErrorCode::kConsumerBusy;
  if (code == uring::UringErrorCode::kInvalid || code == uring::UringErrorCode::kInvalidToken)
    return ErrorCode::kInvalidArgument;
  if (code == uring::UringErrorCode::kShuttingDown || code == uring::UringErrorCode::kCanceled) return ErrorCode::kShuttingDown;
  if (code == uring::UringErrorCode::kPermissionDenied || code == uring::UringErrorCode::kNotImplemented ||
      code == uring::UringErrorCode::kNotSupported || code == uring::UringErrorCode::kAccessDenied ||
      code == uring::UringErrorCode::kUnsupportedKernelVersion)
    return ErrorCode::kProactorUnavailable;
  return ErrorCode::kWriteFailed;
}
}  // namespace zerolog
template <>
struct std::is_error_code_enum<zerolog::ErrorCode> : std::true_type {};
