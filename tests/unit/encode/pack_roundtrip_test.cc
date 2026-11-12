#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <zerolog/format/consteval_parser.hpp>

namespace {

struct Msg {
  std::string_view name;
  int id;
};

struct Named {
  std::string name;
};

enum class Color : unsigned { kRed = 1, kBlue = 2, kCrimson = 1, kNone = 0 };

struct Buf {
  char data[64]{};
  std::size_t len = 0;
  zerolog::abi::OutputAppender out{data, &len, sizeof(data)};
  [[nodiscard]] std::string_view Text() const { return {data, len}; }
};

template <class... Args>
int Render(const zerolog::abi::StaticRecordInfo *info, std::string_view want, int code, const Args &...args) {
  std::byte raw[128]{};
  if (zerolog::encode::EncodeRecord(raw, sizeof(raw), args...) != zerolog::ErrorCode::kSuccess) return code;
  const auto size = zerolog::encode::EncodedBytes(args...);
  zerolog::abi::DecodeCursor cursor{raw, static_cast<std::uint32_t>(size), 0};
  Buf buf;
  if (info->decode(info, cursor, buf.out) != zerolog::ErrorCode::kSuccess) return code + 1;
  return buf.Text() == want ? 0 : code + 2;
}

}  // namespace

int main() {
  {
    std::byte raw[32]{};
    std::string owned = "hello";
    Msg msg{owned, 7};
    if (zerolog::encode::EncodeRecord(raw, sizeof(raw), msg) != zerolog::ErrorCode::kSuccess) return 1;
    owned.assign(64, 'Z');
    constexpr auto fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, Msg>{"{}"};
    const auto size = sizeof(std::uint32_t) + 5 + sizeof(int);
    zerolog::abi::DecodeCursor cursor{raw, static_cast<std::uint32_t>(size), 0};
    Buf buf;
    if (fmt.info->decode(fmt.info, cursor, buf.out) != zerolog::ErrorCode::kSuccess) return 2;
    if (buf.Text() != "hello,7") return 3;
  }
  {
    std::byte raw[32]{};
    std::string owned = "xyz";
    const char *text = owned.c_str();
    if (zerolog::encode::EncodeRecord(raw, sizeof(raw), text) != zerolog::ErrorCode::kSuccess) return 4;
    owned.assign(32, 'Q');
    constexpr auto fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, const char *>{"{}"};
    zerolog::abi::DecodeCursor cursor{raw, static_cast<std::uint32_t>(sizeof(std::uint32_t) + 3), 0};
    Buf buf;
    if (fmt.info->decode(fmt.info, cursor, buf.out) != zerolog::ErrorCode::kSuccess) return 5;
    if (buf.Text() != "xyz") return 6;
  }
  {
    std::byte raw[64]{};
    Named named{std::string{"snap"}};
    if (zerolog::encode::EncodeRecord(raw, sizeof(raw), named) != zerolog::ErrorCode::kSuccess) return 7;
    named.name.assign(32, 'Q');
    constexpr auto fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, Named>{"{}"};
    zerolog::abi::DecodeCursor cursor{raw, static_cast<std::uint32_t>(sizeof(std::uint32_t) + 4), 0};
    Buf buf;
    if (fmt.info->decode(fmt.info, cursor, buf.out) != zerolog::ErrorCode::kSuccess) return 8;
    if (buf.Text() != "snap") return 9;
  }
  constexpr auto color_fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, Color>{"{}"};
  if (const int code = Render(color_fmt.info, "kRed", 20, Color::kRed)) return code;
  if (const int code = Render(color_fmt.info, "kRed", 30, Color::kCrimson)) return code;
  if (const int code = Render(color_fmt.info, "kBlue", 40, Color::kBlue)) return code;
  if (const int code = Render(color_fmt.info, "kNone", 50, Color::kNone)) return code;
  if (const int code = Render(color_fmt.info, "7", 60, static_cast<Color>(7))) return code;
  constexpr const char *empty = nullptr;
  constexpr auto empty_fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, const char *>{"[{}]"};
  if (const int code = Render(empty_fmt.info, "[]", 70, empty)) return code;
  struct Pair {
    std::uint32_t a;
    std::uint32_t b;
  };
  std::byte raw[8]{};
  if (zerolog::encode::EncodeRecord(raw, sizeof(raw), Pair{0x11u, 0x22u}) != zerolog::ErrorCode::kSuccess) return 80;
  if (zerolog::encode::EncodedBytes(Pair{0x11u, 0x22u}) != 8) return 81;
  std::uint32_t first = 0;
  std::uint32_t second = 0;
  std::memcpy(&first, raw, 4);
  std::memcpy(&second, raw + 4, 4);
  if (first != 0x11u || second != 0x22u) return 82;
  {
    constexpr auto bob_fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, char[4]>{"{}"};
    if (const int code = Render(bob_fmt.info, "bob", 90, "bob")) return code;
    if (zerolog::encode::EncodedBytes("bob") != sizeof(std::uint32_t) + 3) return 93;
  }
  {
    char name[16] = "alice";
    constexpr auto alice_fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, char[16]>{"{}"};
    if (const int code = Render(alice_fmt.info, "alice", 100, name)) return code;
    if (zerolog::encode::EncodedBytes(name) != sizeof(std::uint32_t) + 5) return 103;
  }
  {
    char full[4] = {'a', 'b', 'c', 'd'};
    constexpr auto full_fmt = zerolog::format::FormatString<zerolog::abi::Level::kInfo, char[4]>{"{}"};
    if (const int code = Render(full_fmt.info, "abcd", 110, full)) return code;
    if (zerolog::encode::EncodedBytes(full) != sizeof(std::uint32_t) + 4) return 113;
  }
  {
    std::byte tiny[4]{};
    const char *text = "bob";
    if (zerolog::encode::EncodeRecord(tiny, sizeof(tiny), text) != zerolog::ErrorCode::kRecordTooLarge) return 120;
    const auto view = zerolog::encode::PrepareArg(text);
    if (view.size() != 3) return 121;
    if (zerolog::encode::Codec<const char *>::Size(view) != sizeof(std::uint32_t) + 3) return 122;
    std::byte raw_view[16]{};
    std::byte *dest = raw_view;
    std::size_t cap = sizeof(raw_view);
    if (zerolog::encode::Codec<const char *>::Encode(dest, cap, view) != zerolog::ErrorCode::kSuccess) return 123;
    if (sizeof(raw_view) - cap != sizeof(std::uint32_t) + 3) return 124;
  }
  {
    std::byte short_fixed[4]{};
    if (zerolog::encode::EncodeRecord(short_fixed, sizeof(short_fixed), std::uint64_t{1}) !=
        zerolog::ErrorCode::kRecordTooLarge)
      return 130;
  }
  return 0;
}
