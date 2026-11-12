#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>
#include <zerolog/format/number_format.hpp>

namespace {

struct Buf {
  char data[64]{};
  std::size_t len = 0;
  zerolog::abi::OutputAppender out{data, &len, sizeof(data)};
  [[nodiscard]] std::string_view Text() const { return {data, len}; }
  void Reset() { len = 0; }
};

int Expect(std::string_view got, std::string_view want, int code) { return got == want ? 0 : code; }

struct Wide {
  char data[640]{};
  std::size_t len = 0;
  zerolog::abi::OutputAppender out{data, &len, sizeof(data)};
  [[nodiscard]] std::string_view Text() const { return {data, len}; }
};

int ExpectFixed(double value, int precision, int code) {
  char expect[640];
  const auto result = std::to_chars(expect, expect + sizeof(expect), value, std::chars_format::fixed, precision);
  if (result.ec != std::errc{}) return code;
  Wide wide;
  const auto status = zerolog::format::FormatFloat(wide.out, value, zerolog::abi::FormatPresentation::kFixedFloat,
                                                   static_cast<std::uint8_t>(precision));
  if (status != zerolog::ErrorCode::kSuccess) return code + 1;
  const auto want = std::string_view{expect, static_cast<std::size_t>(result.ptr - expect)};
  return Expect(wide.Text(), want, code + 2);
}

}  // namespace

int main() {
  using zerolog::format::FormatBinary;
  using zerolog::format::FormatFloat;
  using zerolog::format::FormatHex;
  using zerolog::format::FormatHexLower;
  using zerolog::format::FormatHexUpper;
  using zerolog::format::FormatInt;
  using zerolog::format::FormatUint;
  Buf buf;
  if (FormatUint(buf.out, 0) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "0", 2)) return code;
  buf.Reset();
  if (FormatUint(buf.out, 9) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "9", 3)) return code;
  buf.Reset();
  if (FormatUint(buf.out, 10) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "10", 4)) return code;
  buf.Reset();
  if (FormatUint(buf.out, 99) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "99", 5)) return code;
  buf.Reset();
  if (FormatUint(buf.out, 100) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "100", 6)) return code;
  buf.Reset();
  if (FormatUint(buf.out, std::numeric_limits<std::uint64_t>::max()) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "18446744073709551615", 7)) return code;
  buf.Reset();
  if (FormatInt(buf.out, -42) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "-42", 8)) return code;
  buf.Reset();
  if (FormatInt(buf.out, std::numeric_limits<std::int64_t>::min()) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "-9223372036854775808", 9)) return code;
  buf.Reset();
  if (FormatHexLower(buf.out, 0) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "0", 10)) return code;
  buf.Reset();
  if (FormatHexLower(buf.out, 255) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "ff", 11)) return code;
  buf.Reset();
  if (FormatHex(buf.out, 255) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "ff", 12)) return code;
  buf.Reset();
  if (FormatHexUpper(buf.out, 255) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "FF", 13)) return code;
  buf.Reset();
  if (FormatHexLower(buf.out, 256) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "100", 14)) return code;
  buf.Reset();
  if (FormatBinary(buf.out, 0) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "0", 15)) return code;
  buf.Reset();
  if (FormatBinary(buf.out, 1) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1", 16)) return code;
  buf.Reset();
  if (FormatBinary(buf.out, 5) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "101", 17)) return code;
  buf.Reset();
  if (FormatBinary(buf.out, std::numeric_limits<std::uint64_t>::max()) != zerolog::ErrorCode::kSuccess) return 1;
  if (buf.Text().size() != 64) return 18;
  buf.Reset();
  if (FormatFloat(buf.out, 1.5) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1.5", 19)) return code;
  buf.Reset();
  if (FormatFloat(buf.out, 1.5, zerolog::abi::FormatPresentation::kFixedFloat, 2) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1.50", 20)) return code;
  if (const int code = ExpectFixed(1e300, 2, 30)) return code;
  if (const int code = ExpectFixed(1.0, 80, 40)) return code;
  if (const int code = ExpectFixed(1.0, 255, 50)) return code;
  char tiny[1]{};
  std::size_t tiny_len = 0;
  zerolog::abi::OutputAppender tiny_out{tiny, &tiny_len, sizeof(tiny)};
  if (FormatFloat(tiny_out, 1.5) != zerolog::ErrorCode::kWouldBlock) return 60;
  if (tiny_len != 0) return 61;
  return 0;
}
