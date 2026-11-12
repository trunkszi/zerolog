/** @file @brief Per-second civil timestamp prefix cache and division-free sub-second nanosecond formatter. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <uarch/uarch.hpp>
#include <zerolog/abi/contract.hpp>
#include <zerolog/abi/record_layout.hpp>
#include <zerolog/format/number_format.hpp>

namespace zerolog::format {

/** @brief Per-thread civil prefix `"YYYY-MM-DD HH:MM:SS."` cached for one Unix
 * second. */
class HeaderCache {
 public:
  HeaderCache() noexcept = default;

  /**
   * @brief Append the cached prefix, 9-digit nanoseconds, `[LEVEL]`, and
   * `[tid:slot]`. A hit recomputes nothing with integer division; negative
   * epochs use Euclidean seconds.
   */
  [[nodiscard]] ErrorCode Append(abi::OutputAppender &out, std::int64_t unix_ns, abi::Level level, std::uint32_t tid,
                                 std::uint32_t slot) noexcept {
    std::uint32_t nanos = 0;
    if (!TryHit(unix_ns, nanos)) {
      nanos = Refresh(unix_ns);
    }
    if (const auto status = out.Append(prefix_, prefix_len_); status != ErrorCode::kSuccess) return status;
    char nanos_text[9];
    WriteNanos(nanos_text, nanos);
    if (const auto status = out.Append(nanos_text, 9); status != ErrorCode::kSuccess) return status;
    if (const auto status = out.Push(' '); status != ErrorCode::kSuccess) return status;
    if (const auto status = out.Push('['); status != ErrorCode::kSuccess) return status;
    const auto name = LevelName(level);
    if (const auto status = out.Append(name.data(), name.size()); status != ErrorCode::kSuccess) return status;
    if (const auto status = out.Append("] [", 3); status != ErrorCode::kSuccess) return status;
    if (const auto status = FormatUint(out, tid); status != ErrorCode::kSuccess) return status;
    if (const auto status = out.Push(':'); status != ErrorCode::kSuccess) return status;
    if (const auto status = FormatUint(out, slot); status != ErrorCode::kSuccess) return status;
    return out.Push(']');
  }

 private:
  static constexpr std::int64_t kNsPerSec = 1'000'000'000;
  static constexpr std::int64_t kSecPerDay = 86'400;

  [[nodiscard, gnu::always_inline]] bool TryHit(std::int64_t unix_ns, std::uint32_t &nanos) const noexcept {
    if (!valid_ || unix_ns < origin_ns_) return false;
    const auto delta = static_cast<std::uint64_t>(unix_ns - origin_ns_);
    if (delta >= static_cast<std::uint64_t>(kNsPerSec)) return false;
    nanos = static_cast<std::uint32_t>(delta);
    return true;
  }

  struct Euclid {
    std::int64_t quot;
    std::int64_t rem;
  };

  /** @brief Toward-zero division adjusted so the remainder is in `[0, d)`. */
  [[nodiscard]] static constexpr Euclid DivEuclid(std::int64_t n, std::int64_t d) noexcept {
    std::int64_t quot = n / d;
    std::int64_t rem = n % d;
    if (rem < 0) {
      rem += d;
      --quot;
    }
    return {quot, rem};
  }

  /** @brief Cold calendar math. The hit path never inlines this division. */
  [[nodiscard, gnu::cold, gnu::noinline]] std::uint32_t Refresh(std::int64_t unix_ns) noexcept {
    const Euclid split = DivEuclid(unix_ns, kNsPerSec);
    constexpr auto min_sec = std::numeric_limits<std::int64_t>::min() / kNsPerSec;
    constexpr auto max_sec = std::numeric_limits<std::int64_t>::max() / kNsPerSec;
    if (split.quot >= min_sec && split.quot <= max_sec) {
      origin_ns_ = split.quot * kNsPerSec;
      valid_ = true;
    } else {
      valid_ = false;
    }
    FillPrefix(split.quot);
    return static_cast<std::uint32_t>(split.rem);
  }

  static void CivilFromDays(std::int64_t z_in, std::int64_t &year, unsigned &month, unsigned &day) noexcept {
    const std::int64_t z = z_in + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    year = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = doy - (153 * mp + 2) / 5 + 1;
    month = mp < 10 ? mp + 3 : mp - 9;
    year += (month <= 2) ? 1 : 0;
  }

  void FillPrefix(std::int64_t seconds) noexcept {
    const Euclid split = DivEuclid(seconds, kSecPerDay);
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    CivilFromDays(split.quot, year, month, day);
    const auto hour = static_cast<unsigned>(split.rem / 3600);
    const auto minute = static_cast<unsigned>((split.rem % 3600) / 60);
    const auto second = static_cast<unsigned>(split.rem % 60);
    char *cursor = prefix_;
    WriteYear(cursor, year);
    *cursor++ = '-';
    Write2(cursor, month);
    *cursor++ = '-';
    Write2(cursor, day);
    *cursor++ = ' ';
    Write2(cursor, hour);
    *cursor++ = ':';
    Write2(cursor, minute);
    *cursor++ = ':';
    Write2(cursor, second);
    *cursor++ = '.';
    prefix_len_ = static_cast<std::uint8_t>(cursor - prefix_);
  }

  static void Write2(char *&cursor, unsigned value) noexcept {
    cursor[0] = kTwoDigits[value * 2];
    cursor[1] = kTwoDigits[value * 2 + 1];
    cursor += 2;
  }

  static void WriteYear(char *&cursor, std::int64_t year) noexcept {
    if (year >= 0 && year <= 9999) {
      Write2(cursor, static_cast<unsigned>(year) / 100);
      Write2(cursor, static_cast<unsigned>(year) % 100);
      return;
    }
    std::uint64_t mag = 0;
    if (year < 0) {
      *cursor++ = '-';
      mag = static_cast<std::uint64_t>(-(year + 1)) + 1;
    } else {
      mag = static_cast<std::uint64_t>(year);
    }
    char digits[20];
    int count = 0;
    if (mag == 0) digits[count++] = '0';
    while (mag != 0) {
      digits[count++] = static_cast<char>('0' + mag % 10);
      mag /= 10;
    }
    while (count > 0) *cursor++ = digits[--count];
  }

  static void WriteNanos(char *text, std::uint32_t nanos) noexcept {
    constexpr auto div100 = uarch::arithmetic::divisor_u32<100>();
    std::uint32_t pair[4];
    for (int i = 3; i >= 0; --i) {
      const auto quot = div100.div(nanos);
      pair[i] = nanos - quot * 100;
      nanos = quot;
    }
    text[0] = static_cast<char>('0' + nanos);
    for (int i = 0; i < 4; ++i) {
      text[1 + i * 2] = kTwoDigits[pair[i] * 2];
      text[2 + i * 2] = kTwoDigits[pair[i] * 2 + 1];
    }
  }

  [[nodiscard]] static constexpr std::string_view LevelName(abi::Level level) noexcept {
    switch (level) {
      case abi::Level::kTrace:
        return "TRACE";
      case abi::Level::kDebug:
        return "DEBUG";
      case abi::Level::kInfo:
        return "INFO";
      case abi::Level::kWarn:
        return "WARN";
      case abi::Level::kError:
        return "ERROR";
      case abi::Level::kOff:
        return "OFF";
    }
    return "OFF";
  }

  std::int64_t origin_ns_{0};
  bool valid_{false};
  std::uint8_t prefix_len_{0};
  char prefix_[40]{};
};

}  // namespace zerolog::format
