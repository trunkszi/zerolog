#include <zerolog/error/zerolog_error.hpp>

#include "support/check.hpp"
int main() {
  using namespace zerolog;
  for (int i = 0; i <= static_cast<int>(ErrorCode::kInvalidArgument); ++i) {
    std::error_code ec = static_cast<ErrorCode>(i);
    CHECK(ec.category() == Category());
    CHECK(!ec.message().empty());
    CHECK(ec.value() == i);
  }
  CHECK(Category().message(256) == "unknown zerolog error");
  CHECK(Category().message(-1) == "unknown zerolog error");
  CHECK(FromUring(uring::UringErrorCode::kAgain) == ErrorCode::kWouldBlock);
  CHECK(FromUring(uring::UringErrorCode::kInvalid) == ErrorCode::kInvalidArgument);
  CHECK(FromUring(uring::UringErrorCode::kPermissionDenied) == ErrorCode::kProactorUnavailable);
  constexpr uring::UringErrorCode unavailable[] = {
      uring::UringErrorCode::kPermissionDenied, uring::UringErrorCode::kNotImplemented, uring::UringErrorCode::kNotSupported,
      uring::UringErrorCode::kAccessDenied, uring::UringErrorCode::kUnsupportedKernelVersion};
  for (auto code : unavailable) CHECK(FromUring(code) == ErrorCode::kProactorUnavailable);
  constexpr uring::UringErrorCode io[] = {uring::UringErrorCode::kIo,      uring::UringErrorCode::kNoSpace,
                                          uring::UringErrorCode::kBadFd,   uring::UringErrorCode::kBrokenPipe,
                                          uring::UringErrorCode::kFault,   uring::UringErrorCode::kNoMemory,
                                          uring::UringErrorCode::kOverflow};
  for (auto code : io) CHECK(FromUring(code) == ErrorCode::kWriteFailed);
  Result<int> result = 42;
  CHECK(*result == 42);
}
