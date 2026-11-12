#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <zerolog/format/output_buffer.hpp>

#include "support/check.hpp"
using namespace zerolog;
static ErrorCode Repeat(const abi::StaticRecordInfo*, abi::DecodeCursor cursor, abi::OutputAppender& out) noexcept {
  if (cursor.remaining != 4) return ErrorCode::kInvalidArgument;
  std::uint32_t count{};
  std::memcpy(&count, cursor.data, 4);
  std::array<char, 4096> bytes{};
  bytes.fill('x');
  while (count != 0) {
    const auto n = std::min(count, 4096U);
    if (const auto error = out.Append(bytes.data(), n); error != ErrorCode::kSuccess) return error;
    count -= n;
  }
  return ErrorCode::kSuccess;
}
static ErrorCode BadDecode(const abi::StaticRecordInfo*, abi::DecodeCursor, abi::OutputAppender& out) noexcept {
  const auto error = out.Append("partial", 7);
  return error == ErrorCode::kSuccess ? ErrorCode::kInvalidArgument : error;
}
int main() {
  auto output = format::OutputBuffer::Create();
  CHECK(output);
  abi::StaticRecordInfo info{};
  info.decode = &Repeat;
  abi::RecordPrefix prefix{abi::RecordHeaderWord{}.with<^^abi::RecordHeaderSpec::level>(abi::Level::kInfo).raw(), &info, 0, 42};
  std::uint32_t count = 0;
  auto append_record = [&]() noexcept {
    return output->FormatRecord(prefix, {reinterpret_cast<const std::byte*>(&count), 4, 0}, 0, 1, 2);
  };
  CHECK(append_record() == ErrorCode::kSuccess);
  const auto overhead = static_cast<std::uint32_t>(output->PendingBytes());
  CHECK(std::string_view(output->Pending().data(), output->Pending().size()).find("[span:42]") != std::string_view::npos);
  auto first = output->Freeze();
  CHECK(first);
  auto done = output->CompleteFrozen(*first, first->length);
  CHECK(done && !*done);
  count = format::kMaxPendingBytes;
  CHECK(append_record() == ErrorCode::kRecordTooLarge && output->TotalUnflushedBytes() == 0);
  count = format::kMaxPendingBytes - overhead;
  CHECK(append_record() == ErrorCode::kSuccess && output->PendingBytes() == format::kMaxPendingBytes);
  auto slab = output->Freeze();
  CHECK(slab && output->FrozenBytes() == format::kMaxPendingBytes);
  count = 0;
  CHECK(append_record() == ErrorCode::kWouldBlock && output->PendingBytes() == 0);
  CHECK(!output->CompleteFrozen(*slab, slab->length + 1));
  auto partial = output->CompleteFrozen(*slab, slab->length - 2);
  CHECK(partial && *partial && output->FrozenBytes() == 2);
  CHECK(!output->CompleteFrozen(*slab, 1));
  count = format::kMaxPendingBytes - 2 - overhead;
  CHECK(append_record() == ErrorCode::kSuccess && output->TotalUnflushedBytes() == format::kMaxPendingBytes);
  const auto suffix = **partial;
  const std::array<char, 2> expected{suffix.data[suffix.offset], suffix.data[suffix.offset + 1]};
  CHECK(output->RestoreFailed(suffix) == ErrorCode::kSuccess);
  CHECK(output->PendingBytes() == format::kMaxPendingBytes && output->FrozenBytes() == 0);
  CHECK(output->Pending()[0] == expected[0] && output->Pending()[1] == expected[1]);
  auto merged = output->Freeze();
  CHECK(merged);
  CHECK(output->CompleteFrozen(*merged, merged->length));
  count = 3;
  CHECK(append_record() == ErrorCode::kSuccess);
  const auto saved = output->PendingBytes();
  info.decode = &BadDecode;
  CHECK(append_record() == ErrorCode::kInvalidArgument && output->PendingBytes() == saved);
  CHECK(output->Rollback(saved + 1) == ErrorCode::kInvalidArgument);
  CHECK(output->Rollback(0) == ErrorCode::kSuccess);
  prefix.info = nullptr;
  CHECK(append_record() == ErrorCode::kInvalidArgument && output->TotalUnflushedBytes() == 0);
}
