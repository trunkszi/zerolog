#include <array>
#include <cstddef>
#include <cstdint>
#include <uarch/probes/gate.hpp>
#include <zerolog/encode/pack_encoder.hpp>

extern "C" {
[[gnu::noinline]] std::uint64_t ZerologProbeInfoU64x2(std::byte *dest, std::uint64_t a, std::uint64_t b) noexcept {
  constexpr std::size_t bytes = zerolog::encode::EncodedBytes(std::uint64_t{}, std::uint64_t{});
  if (zerolog::encode::EncodeRecord(dest, bytes, a, b) != zerolog::ErrorCode::kSuccess) return 0;
  return bytes;
}
}

namespace zerolog::probes {
using namespace uarch::probes;
inline constexpr std::array kGates{
    Gate{"ZerologProbeInfoU64x2", 0, Div | Idiv | Udivti3 | Movabs | Call, 64},
};
}  // namespace zerolog::probes
