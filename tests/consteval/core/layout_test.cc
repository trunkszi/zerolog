#include <type_traits>
#include <uarch/uarch.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/lease.hpp>
#include <zerolog/core/registry.hpp>
#include <zerolog/core/span.hpp>
#include <zerolog/core/spsc_var_queue.hpp>
#include <zerolog/core/thread_buffer.hpp>

namespace {

using namespace zerolog;
using namespace zerolog::core;

static_assert(sizeof(Reservation) == 16 && uarch::NoPadding<Reservation> && uarch::GprPair<Reservation> &&
              uarch::RegisterPassable<Reservation>);
static_assert(sizeof(RecordView) == 16 && uarch::NoPadding<RecordView> && uarch::GprPair<RecordView> &&
              uarch::RegisterPassable<RecordView>);

static_assert(!std::is_copy_constructible_v<SpscVarQueue>);
static_assert(std::is_nothrow_move_constructible_v<SpscVarQueue>);
static_assert(!std::is_copy_constructible_v<ThreadBuffer>);
static_assert(!std::is_move_constructible_v<ThreadBuffer>);
static_assert(!std::is_copy_constructible_v<ThreadRegistry>);
static_assert(!std::is_move_constructible_v<ThreadRegistry>);
static_assert(!std::is_copy_constructible_v<ConsumerLease>);
static_assert(std::is_nothrow_move_constructible_v<ConsumerLease>);
static_assert(!std::is_copy_constructible_v<SpanGuard>);
static_assert(!std::is_move_constructible_v<SpanGuard>);

consteval bool VerifyProducerMetaFastStep() noexcept {
  constexpr auto initial = abi::ProducerMetaWord{}
                               .with_const<^^abi::ProducerMetaSpec::watermark_rung, true>()
                               .with_const<^^abi::ProducerMetaSpec::write_idx, 100U>()
                               .with_const<^^abi::ProducerMetaSpec::free_blocks, 500U>();
  constexpr std::uint32_t blocks = 12U;
  constexpr auto updated = abi::ProducerMetaWord::from_raw(initial.raw() + (static_cast<std::uint64_t>(blocks) << 8) -
                                                           (static_cast<std::uint64_t>(blocks) << 32));
  return updated.test<^^abi::ProducerMetaSpec::watermark_rung>() &&
         !updated.test<^^abi::ProducerMetaSpec::notify_after_publish>() &&
         updated.get<^^abi::ProducerMetaSpec::write_idx>() == 112U &&
         updated.get<^^abi::ProducerMetaSpec::free_blocks>() == 488U;
}

static_assert(VerifyProducerMetaFastStep());
static_assert(kDefaultLevelGate.get<^^abi::LevelGateSpec::min_level>() == abi::Level::kInfo);
static_assert(kDefaultLevelGate.get<^^abi::LevelGateSpec::ring_level>() == abi::Level::kError);
static_assert(kDefaultLevelGate.get<^^abi::LevelGateSpec::configured_flush_level>() == abi::Level::kError);

}  // namespace

int main() { return 0; }
