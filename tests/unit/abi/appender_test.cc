#include <array>
#include <zerolog/abi/record_layout.hpp>

#include "support/check.hpp"
int main() {
  using namespace zerolog;
  std::array<char, 8> bytes{};
  std::size_t length = 0;
  abi::OutputAppender out{bytes.data(), &length, bytes.size()};
  CHECK(out.Remaining() == 8 && out.WritablePtr() == bytes.data());
  CHECK(out.Append("abcdef", 6) == ErrorCode::kSuccess && length == 6);
  CHECK(out.Append("xyz", 3) == ErrorCode::kWouldBlock && length == 6 && bytes[6] == 0);
  CHECK(out.Push('g') == ErrorCode::kSuccess);
  *out.WritablePtr() = 'h';
  out.Advance(1);
  CHECK(out.Remaining() == 0 && length == 8);
  CHECK(out.Push('x') == ErrorCode::kWouldBlock && length == 8);
  CHECK(out.Append("", 0) == ErrorCode::kSuccess);
}
