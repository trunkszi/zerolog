#include <cstddef>
#include <cstdint>
#include <string_view>
#include <zerolog/format/header_cache.hpp>

namespace {

struct Buf {
  char data[96]{};
  std::size_t len = 0;
  zerolog::abi::OutputAppender out{data, &len, sizeof(data)};
  [[nodiscard]] std::string_view Text() const { return {data, len}; }
  void Reset() { len = 0; }
};

int Expect(std::string_view got, std::string_view want, int code) { return got == want ? 0 : code; }

}  // namespace

int main() {
  constexpr std::int64_t ns_per_sec = 1'000'000'000;
  zerolog::format::HeaderCache cache;
  Buf buf;
  if (cache.Append(buf.out, 0, zerolog::abi::Level::kInfo, 1, 2) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1970-01-01 00:00:00.000000000 [INFO] [1:2]", 2)) return code;
  buf.Reset();
  if (cache.Append(buf.out, -1, zerolog::abi::Level::kWarn, 7, 1) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1969-12-31 23:59:59.999999999 [WARN] [7:1]", 3)) return code;
  buf.Reset();
  if (cache.Append(buf.out, 86'400 * ns_per_sec, zerolog::abi::Level::kError, 3, 4) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1970-01-02 00:00:00.000000000 [ERROR] [3:4]", 4)) return code;
  buf.Reset();
  if (cache.Append(buf.out, 5 * ns_per_sec, zerolog::abi::Level::kDebug, 8, 9) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1970-01-01 00:00:05.000000000 [DEBUG] [8:9]", 5)) return code;
  buf.Reset();
  if (cache.Append(buf.out, 1 * ns_per_sec + 42, zerolog::abi::Level::kTrace, 8, 9) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1970-01-01 00:00:01.000000042 [TRACE] [8:9]", 6)) return code;
  buf.Reset();
  if (cache.Append(buf.out, 1 * ns_per_sec + 43, zerolog::abi::Level::kOff, 8, 9) != zerolog::ErrorCode::kSuccess) return 1;
  if (const int code = Expect(buf.Text(), "1970-01-01 00:00:01.000000043 [OFF] [8:9]", 7)) return code;
  return 0;
}
