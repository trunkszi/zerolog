#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <zerolog/format/consteval_parser.hpp>

namespace {

struct Buf {
  char data[64]{};
  std::size_t len = 0;
  zerolog::abi::OutputAppender out{data, &len, sizeof(data)};
  [[nodiscard]] std::string_view Text() const { return {data, len}; }
};

template <class... Args>
int Render(const zerolog::abi::StaticRecordInfo *info, std::string_view want, int code, const Args &...args) {
  std::byte raw[64]{};
  if (zerolog::encode::EncodeRecord(raw, sizeof(raw), args...) != zerolog::ErrorCode::kSuccess) return code;
  const auto size = zerolog::encode::EncodedBytes(args...);
  zerolog::abi::DecodeCursor cursor{raw, static_cast<std::uint32_t>(size), 0};
  Buf buf;
  if (info->decode(info, cursor, buf.out) != zerolog::ErrorCode::kSuccess) return code + 1;
  return buf.Text() == want ? 0 : code + 2;
}

template <class... A>
constexpr auto LogCall(
    zerolog::format::FormatString<zerolog::abi::Level::kInfo, std::type_identity_t<std::remove_cvref_t<A>>...> fmt,
    const A &...) {
  return fmt;
}

}  // namespace

int main() {
  constexpr auto nums = zerolog::format::FormatString<zerolog::abi::Level::kInfo, unsigned, double>{"{:x}:{:.2f}"};
  if (const int code = Render(nums.info, "ff:1.50", 10, 255U, 1.5)) return code;
  constexpr auto escaped = zerolog::format::FormatString<zerolog::abi::Level::kInfo>{"{{ok}}"};
  if (const int code = Render(escaped.info, "{ok}", 20)) return code;
  constexpr auto upper = zerolog::format::FormatString<zerolog::abi::Level::kInfo, int, int>{"{:X} {:b}"};
  if (const int code = Render(upper.info, "A 101", 30, 10, 5)) return code;
  constexpr auto deduced = LogCall("x={} name={}", 42, "bob");
  if (deduced.info->arg_count != 2 || deduced.info->segment_count != 4) return 40;
  if (deduced.info->segment_count == zerolog::abi::kMaxFormatSegments) return 41;
  constexpr auto one_arg = LogCall("{}", 1);
  if (one_arg.info->segment_count != 1 || one_arg.info->segment_count == zerolog::abi::kMaxFormatSegments ||
      one_arg.info->segment_count == deduced.info->segment_count)
    return 42;
  if (const int code = Render(deduced.info, "x=42 name=bob", 50, 42, "bob")) return code;
  return 0;
}
