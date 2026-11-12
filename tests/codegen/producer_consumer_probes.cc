#include <array>
#include <uarch/probes/gate.hpp>
#include <zerolog/core/producer.hpp>
extern "C" {
[[gnu::noinline]] zerolog::ErrorCode ZerologProbeInfoU64x2(std::uint64_t first, std::uint64_t second) noexcept {
  return zerolog::TryInfo("{} {}", first, second);
}
[[gnu::cold, gnu::noinline]] zerolog::ErrorCode ZerologProbeRegisterCold() noexcept {
  auto registered = zerolog::core::detail::ProducerRegisterSlow();
  return registered ? zerolog::ErrorCode::kSuccess : registered.error();
}
}
namespace zerolog::probes {
using namespace uarch::probes;
inline constexpr std::array kGates{
    Gate{"ZerologProbeInfoU64x2", 0, Call | Div | Idiv | Movabs | Stack | Udivti3, 64, AbiRule::Registers},
    Gate{"ZerologProbeRegisterCold", 0, Div | Idiv, 32, AbiRule::Registers, ".text.unlikely"}};
}  // namespace zerolog::probes
