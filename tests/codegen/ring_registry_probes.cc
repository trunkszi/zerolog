#include <array>
#include <cstddef>
#include <cstdint>
#include <uarch/probes/gate.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/registry.hpp>
#include <zerolog/core/spsc_var_queue.hpp>
#include <zerolog/core/thread_buffer.hpp>

extern "C" {

[[gnu::noinline]] std::byte* ZerologProbeTryReserve(zerolog::core::SpscVarQueue* queue, std::uint32_t blocks) noexcept {
  return queue->TryReserve(blocks);
}

[[gnu::noinline]] void ZerologProbePublish(std::byte* record, std::uint64_t raw_header) noexcept {
  zerolog::core::SpscVarQueue::Publish(record, zerolog::abi::RecordHeaderWord::from_raw(raw_header));
}

[[gnu::noinline]] std::byte* ZerologProbeReservationAbi(zerolog::core::Reservation r) noexcept { return r.record + r.blocks; }

[[gnu::noinline]] const std::byte* ZerologProbeRecordViewAbi(zerolog::core::RecordView v) noexcept {
  return v.data + v.header.get<^^zerolog::abi::RecordHeaderSpec::blocks>();
}

[[gnu::cold, gnu::noinline]] std::byte* ZerologProbeReserveSlow(zerolog::core::SpscVarQueue* queue,
                                                                std::uint32_t blocks) noexcept {
  return queue->ReserveSlow(blocks);
}

[[gnu::cold, gnu::noinline]] void ZerologProbeRingDoorbellCold() noexcept {
  zerolog::core::RingDoorbellCold();
  zerolog::core::RingDoorbellUrgentCold();
}

[[gnu::cold, gnu::noinline]] zerolog::core::ThreadBuffer* ZerologProbeRegisterThreadSlow() noexcept {
  return zerolog::core::RegisterThreadSlow();
}

}  // extern "C"

namespace zerolog::probes {
using namespace uarch::probes;
inline constexpr std::array kGates{
    Gate{"ZerologProbeTryReserve", 0, Movabs | Div | Idiv | Udivti3 | Stack | Call, 64, AbiRule::Registers, {}, 8.0},
    Gate{"ZerologProbePublish", 0, Movabs | Div | Idiv | Udivti3 | Stack | Call, 16, AbiRule::Registers, {}, 4.0},
    Gate{"ZerologProbeReservationAbi", 0, Div | Idiv | Udivti3 | Stack | Call, 16, AbiRule::Registers, {}, 4.0},
    Gate{"ZerologProbeRecordViewAbi", 0, Div | Idiv | Udivti3 | Stack | Call, 16, AbiRule::Registers, {}, 4.0},
    Gate{"ZerologProbeReserveSlow", 0, Div | Idiv | Udivti3, 256, AbiRule::Registers, ".text.unlikely"},
    Gate{"ZerologProbeRingDoorbellCold", 0, Div | Idiv | Udivti3, 128, AbiRule::Registers, ".text.unlikely"},
    Gate{"ZerologProbeRegisterThreadSlow", 0, Div | Idiv | Udivti3, 128, AbiRule::Registers, ".text.unlikely"},
    Gate{"_ZN7zerolog4core12SpscVarQueue11ReserveSlowEj", 0, Div | Idiv | Udivti3, 256, AbiRule::None, ".text.unlikely"},
    Gate{"_ZN7zerolog4core16RingDoorbellColdEv", 0, Div | Idiv | Udivti3, 128, AbiRule::None, ".text.unlikely"},
    Gate{"_ZN7zerolog4core22RingDoorbellUrgentColdEv", 0, Div | Idiv | Udivti3, 128, AbiRule::None, ".text.unlikely"},
    Gate{"_ZN7zerolog4core18RegisterThreadSlowEv", 0, Div | Idiv | Udivti3, 128, AbiRule::None, ".text.unlikely"},
};
}  // namespace zerolog::probes
