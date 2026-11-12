#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <uarch/probes/gate.hpp>
#include <zerolog/core/spsc_var_queue.hpp>
#include <zerolog/encode/pack_encoder.hpp>
#include <zerolog/format/consteval_parser.hpp>
#include <zerolog/format/header_cache.hpp>
#include <zerolog/probes/hot_path_probes.hpp>

namespace {
[[nodiscard, gnu::always_inline]] inline std::uint64_t PackEncode(zerolog::ErrorCode code, std::uint32_t bytes) noexcept {
  return static_cast<std::uint64_t>(code) | (static_cast<std::uint64_t>(bytes) << 8);
}
}  // namespace

extern "C" {
std::uint64_t ZerologProbeInfoU64x2(std::byte* dest, std::uint64_t a, std::uint64_t b) noexcept {
  if (dest == nullptr) return PackEncode(zerolog::ErrorCode::kInvalidArgument, 0);
  constexpr std::size_t bytes = zerolog::encode::EncodedBytes(std::uint64_t{}, std::uint64_t{});
  const auto status = zerolog::encode::EncodeRecord(dest, bytes, a, b);
  if (status != zerolog::ErrorCode::kSuccess) return PackEncode(status, 0);
  return PackEncode(status, static_cast<std::uint32_t>(bytes));
}

std::uint32_t ZerologProbeDecodeU64x2(char* dest, std::size_t cap, std::uint64_t a, std::uint64_t b) noexcept {
  if (dest == nullptr) return static_cast<std::uint32_t>(zerolog::ErrorCode::kInvalidArgument);
  alignas(std::uint64_t) std::byte payload[sizeof(std::uint64_t) * 2];
  const auto encoded = ZerologProbeInfoU64x2(payload, a, b);
  const auto status = static_cast<zerolog::ErrorCode>(encoded & 0xffU);
  if (status != zerolog::ErrorCode::kSuccess) return static_cast<std::uint32_t>(status);
  constexpr zerolog::format::FormatString<zerolog::abi::Level::kInfo, std::type_identity_t<std::uint64_t>,
                                          std::type_identity_t<std::uint64_t>>
      format{"{} {}"};
  std::size_t length = 0;
  zerolog::abi::OutputAppender out{dest, &length, cap};
  const zerolog::abi::DecodeCursor cursor{payload, static_cast<std::uint32_t>(encoded >> 8), 0};
  return static_cast<std::uint32_t>(zerolog::encode::DecodeRecord<std::uint64_t, std::uint64_t>(format.info, cursor, out));
}

std::uint32_t ZerologProbeHeaderCacheHit(char* dest, std::size_t cap, std::int64_t unix_ns) noexcept {
  if (dest == nullptr) return static_cast<std::uint32_t>(zerolog::ErrorCode::kInvalidArgument);
  zerolog::format::HeaderCache cache;
  std::size_t length = 0;
  zerolog::abi::OutputAppender out{dest, &length, cap};
  if (const auto status = cache.Append(out, unix_ns, zerolog::abi::Level::kInfo, 1, 0);
      status != zerolog::ErrorCode::kSuccess) {
    return static_cast<std::uint32_t>(status);
  }
  return static_cast<std::uint32_t>(cache.Append(out, unix_ns + 1, zerolog::abi::Level::kInfo, 1, 0));
}

std::uint32_t ZerologProbeSinkOnCompletions(void (*on_completions)(void* sink, const void* first, std::uint32_t count) noexcept,
                                            void* sink, const void* first, std::uint32_t count) noexcept {
  if (on_completions == nullptr || (count != 0 && first == nullptr)) {
    return static_cast<std::uint32_t>(zerolog::ErrorCode::kInvalidArgument);
  }
  on_completions(sink, first, count);
  return static_cast<std::uint32_t>(zerolog::ErrorCode::kSuccess);
}

std::byte* ZerologProbeTryReserve(zerolog::core::SpscVarQueue* queue, std::uint32_t blocks) noexcept {
  return queue->TryReserve(blocks);
}

std::uint32_t ZerologProbePublish(std::byte* record, std::uint64_t header) noexcept {
  if (record == nullptr) return static_cast<std::uint32_t>(zerolog::ErrorCode::kInvalidArgument);
  zerolog::core::SpscVarQueue::Publish(record, zerolog::abi::RecordHeaderWord::from_raw(header));
  return static_cast<std::uint32_t>(zerolog::ErrorCode::kSuccess);
}
}

namespace zerolog::probes {
using namespace uarch::probes;
inline constexpr std::array kGates{
    Gate{"ZerologProbeInfoU64x2", 0, Div | Idiv | Udivti3 | Movabs | Call, 64},
    Gate{"ZerologProbeTryReserve", 0, Div | Idiv | Udivti3 | Movabs | Call | Stack, 64},
};
}  // namespace zerolog::probes
