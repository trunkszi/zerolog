/** @file @brief Atomic level gate, deep-park protocol and futex2 consumer doorbell. */
#pragma once

#include <atomic>
#include <cstdint>
#include <uarch/uarch.hpp>
#include <uring/futex2/futex2.hpp>
#include <uring/sys/syscall.hpp>
#include <zerolog/abi/contract.hpp>
#include <zerolog/error/zerolog_error.hpp>

namespace zerolog::core {
inline constexpr std::int64_t kSysMembarrier = 324;
inline constexpr std::uint32_t kMembarrierCmdPrivateExpedited = 1U << 3;
inline constexpr std::uint32_t kMembarrierCmdRegisterPrivateExpedited = 1U << 4;

/** @brief Register the calling process for private expedited barriers. */
[[nodiscard]] inline Result<void> RegisterPrivateExpeditedMembarrier() noexcept {
  const auto result = uring::sys::Syscall6(kSysMembarrier, kMembarrierCmdRegisterPrivateExpedited);
  if (result < 0) return std::unexpected(ErrorCode::kMembarrierUnavailable);
  return {};
}
/** @brief Order producer publication against a previously registered consumer park. */
[[nodiscard]] inline Result<void> PrivateExpeditedMembarrier() noexcept {
  const auto result = uring::sys::Syscall6(kSysMembarrier, kMembarrierCmdPrivateExpedited);
  if (result < 0) return std::unexpected(ErrorCode::kMembarrierUnavailable);
  return {};
}

/** @brief Default initial LevelGateWord (min_level=kInfo, ring_level=kError, configured_flush_level=kError). */
inline constexpr abi::LevelGateWord kDefaultLevelGate =
    abi::LevelGateWord{}
        .with_const<^^abi::LevelGateSpec::min_level, abi::Level::kInfo>()
        .with_const<^^abi::LevelGateSpec::ring_level, abi::Level::kError>()
        .with_const<^^abi::LevelGateSpec::configured_flush_level, abi::Level::kError>();

inline constinit uarch::Isolated<std::atomic<std::uint32_t>> g_level_gate{kDefaultLevelGate.raw()};
inline constinit uarch::Isolated<uring::futex2::FutexWord32> g_doorbell{};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);

/** @brief Load the packed 32-bit LevelGateWord with the requested memory ordering. */
[[nodiscard, gnu::always_inline]] inline abi::LevelGateWord LoadLevelGate(
    std::memory_order order = std::memory_order_relaxed) noexcept {
  return abi::LevelGateWord::from_raw(g_level_gate->load(order));
}

/** @brief Atomically update min_level and increment the gate epoch without clobbering other fields. */
inline abi::LevelGateWord SetLevel([[maybe_unused]] abi::Level level) noexcept {
  std::uint32_t raw = g_level_gate->load(std::memory_order_acquire);
  for (;;) {
    const auto old_word = abi::LevelGateWord::from_raw(raw);
    const auto next_word =
        old_word.with<^^abi::LevelGateSpec::min_level>(level).wrapping_add_top_field<^^abi::LevelGateSpec::epoch>(1U);
    if (g_level_gate->compare_exchange_weak(raw, next_word.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return next_word;
    }
  }
}

/** @brief Return the current minimum log level threshold. */
[[nodiscard, gnu::always_inline]] inline abi::Level GetLevel() noexcept {
  return LoadLevelGate(std::memory_order_relaxed).get<^^abi::LevelGateSpec::min_level>();
}

/** @brief Atomically update configured_flush_level (and ring_level when not in deep park). */
inline abi::LevelGateWord FlushOn([[maybe_unused]] abi::Level level) noexcept {
  std::uint32_t raw = g_level_gate->load(std::memory_order_acquire);
  for (;;) {
    const auto old_word = abi::LevelGateWord::from_raw(raw);
    const auto effective_ring = old_word.test<^^abi::LevelGateSpec::deep_park>() ? abi::Level::kTrace : level;
    const auto next_word = old_word.with<^^abi::LevelGateSpec::configured_flush_level>(level)
                               .with<^^abi::LevelGateSpec::ring_level>(effective_ring)
                               .wrapping_add_top_field<^^abi::LevelGateSpec::epoch>(1U);
    if (g_level_gate->compare_exchange_weak(raw, next_word.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return next_word;
    }
  }
}

/** @brief Return the configured flush level threshold. */
[[nodiscard, gnu::always_inline]] inline abi::Level GetFlushLevel() noexcept {
  return LoadLevelGate(std::memory_order_relaxed).get<^^abi::LevelGateSpec::configured_flush_level>();
}

/** @brief Atomically set or clear the has_sinks bit in the global LevelGateWord. */
inline abi::LevelGateWord SetHasSinks([[maybe_unused]] bool has_sinks) noexcept {
  std::uint32_t raw = g_level_gate->load(std::memory_order_acquire);
  for (;;) {
    const auto old_word = abi::LevelGateWord::from_raw(raw);
    const auto next_word =
        old_word.with<^^abi::LevelGateSpec::has_sinks>(has_sinks).wrapping_add_top_field<^^abi::LevelGateSpec::epoch>(1U);
    if (g_level_gate->compare_exchange_weak(raw, next_word.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return next_word;
    }
  }
}

/** @brief Enter DeepPark by setting deep_park=true and lowering ring_level to kTrace. */
inline abi::LevelGateWord EnterDeepPark() noexcept {
  std::uint32_t raw = g_level_gate->load(std::memory_order_acquire);
  for (;;) {
    const auto old_word = abi::LevelGateWord::from_raw(raw);
    const auto next_word = old_word.with<^^abi::LevelGateSpec::deep_park>(true)
                               .with<^^abi::LevelGateSpec::ring_level>(abi::Level::kTrace)
                               .wrapping_add_top_field<^^abi::LevelGateSpec::epoch>(1U);
    if (g_level_gate->compare_exchange_weak(raw, next_word.raw(), std::memory_order_seq_cst, std::memory_order_acquire)) {
      return next_word;
    }
  }
}

/** @brief Exit DeepPark by clearing deep_park and restoring ring_level to configured_flush_level. */
inline abi::LevelGateWord ExitDeepPark() noexcept {
  std::uint32_t raw = g_level_gate->load(std::memory_order_acquire);
  for (;;) {
    const auto old_word = abi::LevelGateWord::from_raw(raw);
    const auto configured = old_word.get<^^abi::LevelGateSpec::configured_flush_level>();
    const auto next_word = old_word.with<^^abi::LevelGateSpec::deep_park>(false)
                               .with<^^abi::LevelGateSpec::ring_level>(configured)
                               .wrapping_add_top_field<^^abi::LevelGateSpec::epoch>(1U);
    if (g_level_gate->compare_exchange_weak(raw, next_word.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return next_word;
    }
  }
}

/** @brief Load the packed 32-bit DoorbellWord with the requested memory ordering. */
[[nodiscard, gnu::always_inline]] inline abi::DoorbellWord LoadDoorbell(
    std::memory_order order = std::memory_order_acquire) noexcept {
  return abi::DoorbellWord::from_raw(std::atomic_ref<std::uint32_t>{g_doorbell->value}.load(order));
}

/** @brief Attempt to transition the doorbell into parked state if @p expected has not changed. */
[[nodiscard]] inline bool TryParkDoorbell(abi::DoorbellWord& expected) noexcept {
  std::uint32_t raw_expected = expected.raw();
  const auto desired = expected.with<^^abi::DoorbellSpec::parked>(true).with<^^abi::DoorbellSpec::urgent>(false);
  if (std::atomic_ref<std::uint32_t>{g_doorbell->value}.compare_exchange_strong(
          raw_expected, desired.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
    expected = desired;
    return true;
  }
  expected = abi::DoorbellWord::from_raw(raw_expected);
  return false;
}

/** @brief Clear parked and urgent flags on the consumer side and return the previous DoorbellWord. */
inline abi::DoorbellWord ConsumeDoorbellNotification() noexcept {
  std::atomic_ref<std::uint32_t> ref{g_doorbell->value};
  std::uint32_t raw = ref.load(std::memory_order_acquire);
  for (;;) {
    const auto old_word = abi::DoorbellWord::from_raw(raw);
    const auto next_word = old_word.with<^^abi::DoorbellSpec::parked>(false).with<^^abi::DoorbellSpec::urgent>(false);
    if (ref.compare_exchange_weak(raw, next_word.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return old_word;
    }
  }
}

/** @brief Construct a process-private 32-bit futex2 reference to the global consumer doorbell word. */
[[nodiscard, gnu::always_inline]] inline uring::Result<uring::futex2::FutexRef> DoorbellFutexRef() noexcept {
  return uring::futex2::MakeRef(g_doorbell.value, uring::futex2::FutexScope::kProcessPrivate);
}

/** @brief Out-of-line cold helper incrementing ring_seq and waking a parked consumer via futex2. */
[[gnu::cold, gnu::noinline]] inline void RingDoorbellCold() noexcept {
  std::atomic_ref<std::uint32_t> ref{g_doorbell->value};
  std::uint32_t raw = ref.load(std::memory_order_relaxed);
  for (;;) {
    const auto old_word = abi::DoorbellWord::from_raw(raw);
    const auto next_word =
        old_word.with<^^abi::DoorbellSpec::parked>(false).wrapping_add_top_field<^^abi::DoorbellSpec::ring_seq>(1U);
    if (ref.compare_exchange_weak(raw, next_word.raw(), std::memory_order_release, std::memory_order_relaxed)) {
      if (old_word.test<^^abi::DoorbellSpec::parked>()) {
        if (auto futex_ref = uring::futex2::MakeRef(g_doorbell.value, uring::futex2::FutexScope::kProcessPrivate)) {
          // NOLINTNEXTLINE(bugprone-unused-return-value)
          (void)uring::futex2::Wake(*futex_ref, 1);
        }
      }
      return;
    }
  }
}

/** @brief Out-of-line cold helper setting urgent=true, incrementing ring_seq and waking a parked consumer. */
[[gnu::cold, gnu::noinline]] inline void RingDoorbellUrgentCold() noexcept {
  std::atomic_ref<std::uint32_t> ref{g_doorbell->value};
  std::uint32_t raw = ref.load(std::memory_order_relaxed);
  for (;;) {
    const auto old_word = abi::DoorbellWord::from_raw(raw);
    const auto next_word = old_word.with<^^abi::DoorbellSpec::parked>(false)
                               .with<^^abi::DoorbellSpec::urgent>(true)
                               .wrapping_add_top_field<^^abi::DoorbellSpec::ring_seq>(1U);
    if (ref.compare_exchange_weak(raw, next_word.raw(), std::memory_order_release, std::memory_order_relaxed)) {
      if (old_word.test<^^abi::DoorbellSpec::parked>()) {
        if (auto futex_ref = uring::futex2::MakeRef(g_doorbell.value, uring::futex2::FutexScope::kProcessPrivate)) {
          // NOLINTNEXTLINE(bugprone-unused-return-value)
          (void)uring::futex2::Wake(*futex_ref, 1);
        }
      }
      return;
    }
  }
}

/** @brief Reset the global LevelGateWord and DoorbellWord for deterministic unit tests. */
inline void ResetDoorbellForTesting() noexcept {
  g_level_gate->store(kDefaultLevelGate.raw(), std::memory_order_seq_cst);
  std::atomic_ref<std::uint32_t>{g_doorbell->value}.store(0U, std::memory_order_seq_cst);
}

}  // namespace zerolog::core
