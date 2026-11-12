/** @file @brief Allocation-free integer, hexadecimal, binary and floating-point formatters. */
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <type_traits>
#include <uarch/uarch.hpp>
#include <zerolog/abi/record_layout.hpp>

namespace zerolog::format {

/** @brief Two ASCII digits for every value in `0..99`, stored as adjacent
 * bytes. */
inline constexpr char kTwoDigits[] =
    "00010203040506070809101112131415161718192"
    "021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596"
    "061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline constexpr auto kDiv100 = uarch::arithmetic::divisor_u64<100>();
inline constexpr char kHexLower[] = "0123456789abcdef";
inline constexpr char kHexUpper[] = "0123456789ABCDEF";

namespace detail {

[[nodiscard, gnu::always_inline]] inline ErrorCode AppendBytes(abi::OutputAppender &out, const char *data,
                                                               std::size_t size) noexcept {
  return out.Append(data, size);
}

[[nodiscard]] inline ErrorCode AppendReversed(abi::OutputAppender &out, const char *reversed, std::size_t size) noexcept {
  char forward[64];
  for (std::size_t i = 0; i < size; ++i) forward[i] = reversed[size - 1 - i];
  return AppendBytes(out, forward, size);
}

}  // namespace detail

/** @brief Decimal formatting with one store per two digits. */
[[nodiscard]] inline ErrorCode FormatUint(abi::OutputAppender &out, std::uint64_t value) noexcept {
  if (value == 0) return out.Push('0');
  char digits[20];
  std::size_t count = 0;
  while (value >= 100) {
    const auto quot = kDiv100.div(value);
    const auto rem = static_cast<std::uint32_t>(value - quot * 100);
    digits[count++] = kTwoDigits[rem * 2 + 1];
    digits[count++] = kTwoDigits[rem * 2];
    value = quot;
  }
  if (value >= 10) {
    digits[count++] = kTwoDigits[value * 2 + 1];
    digits[count++] = kTwoDigits[value * 2];
  } else {
    digits[count++] = static_cast<char>('0' + value);
  }
  return detail::AppendReversed(out, digits, count);
}

/** @brief Signed decimal. `INT64_MIN` is negated via an unsigned magnitude. */
[[nodiscard]] inline ErrorCode FormatInt(abi::OutputAppender &out, std::int64_t value) noexcept {
  if (value < 0) {
    if (const auto status = out.Push('-'); status != ErrorCode::kSuccess) return status;
    const auto magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1;
    return FormatUint(out, magnitude);
  }
  return FormatUint(out, static_cast<std::uint64_t>(value));
}

namespace detail {

[[nodiscard]] inline ErrorCode FormatHexDigits(abi::OutputAppender &out, std::uint64_t value, const char *alphabet) noexcept {
  if (value == 0) return out.Push('0');
  char digits[16];
  std::size_t count = 0;
  while (value != 0) {
    digits[count++] = alphabet[value & 0xF];
    value >>= 4;
  }
  return AppendReversed(out, digits, count);
}

}  // namespace detail

/** @brief Lower-case hexadecimal without a prefix. */
[[nodiscard]] inline ErrorCode FormatHexLower(abi::OutputAppender &out, std::uint64_t value) noexcept {
  return detail::FormatHexDigits(out, value, kHexLower);
}

/** @brief Upper-case hexadecimal without a prefix. */
[[nodiscard]] inline ErrorCode FormatHexUpper(abi::OutputAppender &out, std::uint64_t value) noexcept {
  return detail::FormatHexDigits(out, value, kHexUpper);
}

/** @brief Ticket alias of `FormatHexLower`. */
[[nodiscard, gnu::always_inline]] inline ErrorCode FormatHex(abi::OutputAppender &out, std::uint64_t value) noexcept {
  return FormatHexLower(out, value);
}

/** @brief Binary digits without a prefix. */
[[nodiscard]] inline ErrorCode FormatBinary(abi::OutputAppender &out, std::uint64_t value) noexcept {
  if (value == 0) return out.Push('0');
  char digits[64];
  std::size_t count = 0;
  while (value != 0) {
    digits[count++] = static_cast<char>('0' + (value & 1U));
    value >>= 1;
  }
  return detail::AppendReversed(out, digits, count);
}

/** @brief `std::to_chars` floating formatting into the appender. `{:.Nf}`
 * selects fixed precision. */
template <class T>
  requires std::is_floating_point_v<T>
[[nodiscard]] inline ErrorCode FormatFloat(abi::OutputAppender &out, T value,
                                           abi::FormatPresentation presentation = abi::FormatPresentation::kDefault,
                                           std::uint8_t precision = 6) noexcept {
  char buffer[640];
  const auto format =
      presentation == abi::FormatPresentation::kFixedFloat ? std::chars_format::fixed : std::chars_format::general;
  const std::to_chars_result result =
      presentation == abi::FormatPresentation::kFixedFloat
          ? std::to_chars(buffer, buffer + sizeof(buffer), value, format, static_cast<int>(precision))
          : std::to_chars(buffer, buffer + sizeof(buffer), value, format);
  if (result.ec != std::errc{}) {
    return result.ec == std::errc::value_too_large ? ErrorCode::kRecordTooLarge : ErrorCode::kInvalidArgument;
  }
  return out.Append(buffer, static_cast<std::size_t>(result.ptr - buffer));
}

}  // namespace zerolog::format
