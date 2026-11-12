#include <array>
#include <uarch/probes/gate.hpp>
#include <zerolog/abi/record_layout.hpp>
#include <zerolog/sys/tsc_clock.hpp>
extern "C" {
[[gnu::noinline]] std::uint64_t ZerologProbeRdtsc() noexcept { return zerolog::sys::ReadTsc(); }
[[gnu::cold, gnu::noinline]] std::uint64_t ZerologProbeCold(std::uint64_t value) noexcept { return value + 1; }
[[gnu::noinline]] std::uint64_t ZerologProbeChoose(std::uint64_t value, std::uint64_t other) noexcept {
  return uarch::predicate(value != 0).select_register(value, other);
}
[[gnu::noinline]] std::uint32_t ZerologProbeHeapAccess(const zerolog::InplaceVector<zerolog::abi::HeapNode, 64>* heap,
                                                       std::size_t index) noexcept {
  return heap->data()[index].slot;
}

[[gnu::noinline]] std::uint64_t ZerologProbeMeta(std::uint64_t raw, std::uint32_t blocks) noexcept {
  auto meta = zerolog::abi::ProducerMetaWord::from_raw(raw);
  return meta.raw() + (std::uint64_t{blocks} << 8) - (std::uint64_t{blocks} << 32);
}
[[gnu::noinline]] zerolog::ErrorCode ZerologProbeAppend(zerolog::abi::OutputAppender* out, const char* src) noexcept {
  return out->Append(src, 8);
}
[[gnu::noinline]] std::uint64_t ZerologProbeTsc(const zerolog::sys::TscClock* clock, std::uint64_t tsc) noexcept {
  return clock->ToWallNanoseconds(tsc);
}
}
namespace zerolog::probes {
using namespace uarch::probes;
inline constexpr std::array kGates{
    Gate{"ZerologProbeCold", 0, Call | Div | Idiv, 8, AbiRule::Registers, ".text.unlikely", 4.0},
    Gate{"ZerologProbeChoose", Cmov, Call | Div | Idiv | Stack, 8, AbiRule::Registers, {}, 4.0},
    Gate{"ZerologProbeHeapAccess", 0, Jcc | Call | Div | Idiv | Stack, 16, AbiRule::None, {}, 4.0, true},
    Gate{"ZerologProbeRdtsc", 0, Call | Stack, 4, AbiRule::Registers, {}, 32.0},
    Gate{"ZerologProbeMeta", 0, Movabs | Div | Idiv | Udivti3 | Stack | Call, 64},
    Gate{"ZerologProbeAppend", 0, Div | Idiv | Udivti3 | Call, 64},
    Gate{"ZerologProbeTsc", 0, Div | Idiv | Udivti3 | Call, 64}};
}  // namespace zerolog::probes
