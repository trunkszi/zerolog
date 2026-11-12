/** @file @brief Frozen state words and register-sized values shared across logging layers. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <uarch/uarch.hpp>
#include <uring/futex2/futex_flags.hpp>

namespace zerolog::abi {
template <class Spec>
consteval bool HotFlagsValid() {
  template for (constexpr auto member :
                std::define_static_array(std::meta::nonstatic_data_members_of(^^Spec, std::meta::access_context::current()))) {
    if constexpr (uarch::reflect::annotation_count_of<uarch::bits::Flag>(member) == 1) {
      if (uarch::reflect::annotation_of<uarch::bits::Flag>(member)->bit >= 31) return false;
    }
  }
  return true;
}

enum class Level : std::uint8_t {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kOff = 5,
};

enum class QueueFullPolicy : std::uint8_t {
  kDrop = 0,
  kBlock = 1,
};

struct LevelGateSpec {
  using Word = std::uint32_t;
  Level min_level[[= uarch::bits::Field{0, 8}]];               /**< Producer admission threshold. */
  Level ring_level[[= uarch::bits::Field{8, 8}]];              /**< Threshold requiring a doorbell notification. */
  Level configured_flush_level[[= uarch::bits::Field{16, 8}]]; /**< Configured threshold restored after deep park. */
  bool deep_park[[= uarch::bits::Flag{24}]];
  bool has_sinks[[= uarch::bits::Flag{25}]];
  std::uint8_t epoch[[= uarch::bits::Field{26, 6}]]; /**< Registration or configuration incarnation. */
};
static_assert(HotFlagsValid<LevelGateSpec>(), "hot Flag bit must be below 31");
using LevelGateWord = uarch::BitWord<LevelGateSpec>;

struct DoorbellSpec {
  using Word = std::uint32_t;
  bool parked[[= uarch::bits::Flag{0}]];
  bool urgent[[= uarch::bits::Flag{1}]];
  std::uint32_t ring_seq[[= uarch::bits::Field{2, 30}]]; /**< Wrapping notification sequence. */
};
static_assert(HotFlagsValid<DoorbellSpec>(), "hot Flag bit must be below 31");
using DoorbellWord = uarch::BitWord<DoorbellSpec>;

inline constexpr std::uint32_t kDoorbellFutexFlags = uring::futex2::Futex2Flags<uring::futex2::FutexScope::kProcessPrivate>();

struct ProducerMetaSpec {
  using Word = std::uint64_t;
  bool watermark_rung[[= uarch::bits::Flag{0}]];
  bool notify_after_publish[[= uarch::bits::Flag{1}]];
  std::uint32_t write_idx[[= uarch::bits::Field{8, 24}]];    /**< Producer-private cursor in eight-byte blocks. */
  std::uint32_t free_blocks[[= uarch::bits::Field{32, 32}]]; /**< Credits available to the producer. */
};
static_assert(HotFlagsValid<ProducerMetaSpec>(), "hot Flag bit must be below 31");
using ProducerMetaWord = uarch::BitWord<ProducerMetaSpec>;

enum class SlotState : std::uint8_t { kFree = 0, kActive = 1, kOrphaned = 2, kDraining = 3 };
struct SlotEpochSpec {
  using Word = std::uint64_t;
  SlotState state[[= uarch::bits::Field{0, 2}]]; /**< Registry slot ownership state. */
  bool overflowed[[= uarch::bits::Flag{2}]];
  std::uint32_t epoch[[= uarch::bits::Field{32, 32}]]; /**< Registration or configuration incarnation. */
};
static_assert(HotFlagsValid<SlotEpochSpec>(), "hot Flag bit must be below 31");
using SlotEpochWord = uarch::BitWord<SlotEpochSpec>;

struct ConsumerStateSpec {
  using Word = std::uint64_t;
  bool active[[= uarch::bits::Flag{0}]];
  bool poisoned[[= uarch::bits::Flag{1}]];
  bool external[[= uarch::bits::Flag{2}]];
  bool poller_running[[= uarch::bits::Flag{3}]];
  std::uint32_t generation[[= uarch::bits::Field{32, 32}]]; /**< Consumer lease incarnation. */
};
static_assert(HotFlagsValid<ConsumerStateSpec>(), "hot Flag bit must be below 31");
using ConsumerStateWord = uarch::BitWord<ConsumerStateSpec>;

struct DrainFlagsSpec {
  using Word = std::uint32_t;
  bool backpressure[[= uarch::bits::Flag{0}]];
  bool urgent_seen[[= uarch::bits::Flag{1}]];
  bool frozen_in_flight[[= uarch::bits::Flag{2}]];
  bool budget_exhausted[[= uarch::bits::Flag{3}]];
  bool more_pending[[= uarch::bits::Flag{4}]];
};
static_assert(HotFlagsValid<DrainFlagsSpec>(), "hot Flag bit must be below 31");
using DrainFlagsWord = uarch::BitWord<DrainFlagsSpec>;

enum class LifecycleState : std::uint8_t { kIdle = 0, kRunning = 1, kStopping = 2, kDetached = 3, kClosed = 4 };
enum class WakeState : std::uint8_t { kIdle = 0, kArmed = 1, kCanceling = 2 };
enum class TimerState : std::uint8_t { kIdle = 0, kArmed = 1, kUpdating = 2, kCanceling = 3 };
enum class WriteRotateState : std::uint8_t {
  kIdle = 0,
  kReady = 1,
  kWriting = 2,
  kRenaming = 3,
  kOpening = 4,
  kClosingOld = 5,
};
struct SinkFlagsSpec {
  using Word = std::uint32_t;
  LifecycleState lifecycle[[= uarch::bits::Field{0, 3}]]; /**< Sink lifetime state. */
  WakeState wake[[= uarch::bits::Field{3, 2}]];           /**< Futex operation state. */
  TimerState timer[[= uarch::bits::Field{5, 2}]];         /**< Timeout operation state. */
  WriteRotateState io[[= uarch::bits::Field{7, 3}]];      /**< Write or rotation operation state. */
  bool rotation_retry[[= uarch::bits::Flag{10}]];
  bool write_retry[[= uarch::bits::Flag{11}]];
  std::uint8_t rename_step[[= uarch::bits::Field{12, 4}]]; /**< Current path-table rotation step. */
};
static_assert(HotFlagsValid<SinkFlagsSpec>(), "hot Flag bit must be below 31");
using SinkFlagsWord = uarch::BitWord<SinkFlagsSpec>;

struct DrainBudget {
  std::uint32_t max_records{4096};      /**< Maximum records formatted by one drain. */
  std::uint32_t max_bytes{1024 * 1024}; /**< Maximum bytes formatted by one drain. */
};
static_assert(sizeof(DrainBudget) == 8 && uarch::NoPadding<DrainBudget> && uarch::RegisterPassable<DrainBudget>);

struct DrainStats {
  std::uint32_t records{};      /**< Records formatted by this drain. */
  std::uint32_t bytes{};        /**< Bytes formatted by this drain. */
  DrainFlagsSpec::Word flags{}; /**< Packed DrainFlagsWord result. */
  std::uint32_t reserved{};     /**< Must remain zero. */
};
static_assert(sizeof(DrainStats) == 16 && uarch::NoPadding<DrainStats> && uarch::GprPair<DrainStats>);

struct HeapNode {
  std::uint64_t tsc{};    /**< Unserialized producer timestamp. */
  std::uint32_t slot{};   /**< Owning registry slot. */
  std::uint32_t blocks{}; /**< Record extent in eight-byte blocks. */
};
static_assert(sizeof(HeapNode) == 16 && uarch::NoPadding<HeapNode> && uarch::GprPair<HeapNode>);
}  // namespace zerolog::abi

namespace zerolog::core {
/** @brief Bounded non-throwing container shared with the microarchitecture primitives. */
template <class T, std::size_t N>
using InplaceVector = uarch::container::freestanding::inplace_vector<T, N>;
}  // namespace zerolog::core
namespace zerolog {
using core::InplaceVector;
}  // namespace zerolog
