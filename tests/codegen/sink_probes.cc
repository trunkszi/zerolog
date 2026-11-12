#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <uarch/probes/gate.hpp>
#include <uring/core/completion.hpp>
#include <uring/core/proactor.hpp>
#include <zerolog/sink/proactor_sink.hpp>

extern "C" {

[[gnu::noinline]] void ZerologProbeSinkOnCompletions(zerolog::sink::ProactorLogSink<uring::ScriptedProactor, 1>* sink,
                                                     const uring::Delivery* deliveries, std::size_t count) noexcept {
  sink->OnCompletions(std::span<const uring::Delivery>{deliveries, count});
}

[[gnu::noinline]] std::uint32_t ZerologProbeIoActionAbi(zerolog::sink::IoAction action, zerolog::RotationPolicy policy,
                                                        zerolog::WriteProgress progress) noexcept {
  return action.length + action.backoff_ms + static_cast<std::uint32_t>(policy.max_bytes) + progress.written;
}

}  // extern "C"

namespace zerolog::probes {
using namespace uarch::probes;
inline constexpr std::array kGates{
    Gate{"ZerologProbeSinkOnCompletions", 0, Div | Idiv | Udivti3, 0, AbiRule::Registers},
    Gate{"ZerologProbeIoActionAbi", 0, Div | Idiv | Udivti3 | Movabs, 32, AbiRule::Registers},
};
}  // namespace zerolog::probes
