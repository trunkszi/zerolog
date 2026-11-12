/** @file @brief 64-slot lock-free thread registry, TLS slot binding and orphan reclamation. */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <uarch/uarch.hpp>
#include <zerolog/abi/contract.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/thread_buffer.hpp>
#include <zerolog/error/zerolog_error.hpp>

namespace zerolog::core {

inline constexpr std::size_t kMaxThreads = 64;

/** @brief Registry slot pairing a cache-isolated SlotEpochWord with its lazily allocated ThreadBuffer. */
struct SlotEntry {
  uarch::Isolated<std::atomic<std::uint64_t>> epoch_word{0U};
  std::atomic<ThreadBuffer*> buffer{nullptr};
};

/** @brief Fixed 64-slot lock-free thread registry managing producer ThreadBuffer lifecycle. */
class ThreadRegistry final {
 public:
  static constexpr std::size_t kMaxSlots = kMaxThreads;

  constexpr ThreadRegistry() noexcept = default;
  ThreadRegistry(const ThreadRegistry&) = delete ("ThreadRegistry is a process-wide singleton");
  ThreadRegistry(ThreadRegistry&&) = delete ("ThreadRegistry is a process-wide singleton");
  ThreadRegistry& operator=(const ThreadRegistry&) = delete;
  ThreadRegistry& operator=(ThreadRegistry&&) = delete;

  /** @brief Claim a free slot and allocate/reuse its ThreadBuffer, returning exact ErrorCode on failure. */
  [[nodiscard, gnu::cold, gnu::noinline]] Result<ThreadBuffer*> TryRegisterSlot(std::uint32_t* out_slot = nullptr) noexcept {
    uarch::Bitmap64 candidates = uarch::Bitmap64::from_raw(~active_mask_->load(std::memory_order_acquire));
    while (auto idx_opt = candidates.pop_lowest_one()) {
      auto res = TryClaimSlot(*idx_opt);
      if (res.has_value()) {
        if (out_slot != nullptr) {
          *out_slot = *idx_opt;
        }
        return *res;
      }
      if (res.error() != ErrorCode::kTooManyThreads) {
        return std::unexpected(res.error());
      }
    }
    for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
      auto res = TryClaimSlot(i);
      if (res.has_value()) {
        if (out_slot != nullptr) {
          *out_slot = i;
        }
        return *res;
      }
      if (res.error() != ErrorCode::kTooManyThreads) {
        return std::unexpected(res.error());
      }
    }
    rejected_threads_->fetch_add(1U, std::memory_order_relaxed);
    return std::unexpected(ErrorCode::kTooManyThreads);
  }

  /** @brief Claim a free slot and return its ThreadBuffer pointer, or nullptr on exhaustion/error. */
  [[nodiscard, gnu::cold, gnu::noinline]] ThreadBuffer* RegisterSlot(std::uint32_t* out_slot = nullptr) noexcept {
    auto res = TryRegisterSlot(out_slot);
    return res ? *res : nullptr;
  }

  /** @brief Transition an active slot to kOrphaned upon thread exit and ring the consumer doorbell. */
  [[gnu::cold, gnu::noinline]] void RetireSlot(std::uint32_t slot) noexcept {
    if (slot >= kMaxSlots) {
      return;
    }
    auto& atom = *slots_[slot].epoch_word;
    std::uint64_t raw = atom.load(std::memory_order_acquire);
    for (;;) {
      const auto word = abi::SlotEpochWord::from_raw(raw);
      if (word.get<^^abi::SlotEpochSpec::state>() != abi::SlotState::kActive) {
        return;
      }
      const auto next = word.with<^^abi::SlotEpochSpec::state>(abi::SlotState::kOrphaned);
      if (atom.compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
        RingDoorbellCold();
        return;
      }
    }
  }

  /** @brief Reclaim an orphaned slot whose SPSC ring has been completely drained, advancing its epoch. */
  [[nodiscard]] bool TryReclaimOrphanedSlot(std::uint32_t slot) noexcept {
    if (slot >= kMaxSlots) {
      return false;
    }
    auto& atom = *slots_[slot].epoch_word;
    std::uint64_t raw = atom.load(std::memory_order_acquire);
    const auto word = abi::SlotEpochWord::from_raw(raw);
    if (word.get<^^abi::SlotEpochSpec::state>() != abi::SlotState::kOrphaned) {
      return false;
    }
    ThreadBuffer* buf = slots_[slot].buffer.load(std::memory_order_acquire);
    if (buf != nullptr && buf->queue.Front().Valid()) {
      return false;
    }
    const auto draining = word.with<^^abi::SlotEpochSpec::state>(abi::SlotState::kDraining);
    if (!atom.compare_exchange_strong(raw, draining.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return false;
    }
    active_mask_->fetch_and(~(1ULL << slot), std::memory_order_release);
    const auto freed = word.with<^^abi::SlotEpochSpec::state>(abi::SlotState::kFree)
                           .with<^^abi::SlotEpochSpec::overflowed>(false)
                           .wrapping_add_top_field<^^abi::SlotEpochSpec::epoch>(1U);
    atom.store(freed.raw(), std::memory_order_release);
    return true;
  }

  /** @brief Return the 64-bit bitmap of active and orphaned slots requiring consumer scanning. */
  [[nodiscard, gnu::always_inline]] uarch::Bitmap64 ActiveSlotsBitmap() const noexcept {
    return uarch::Bitmap64::from_raw(active_mask_->load(std::memory_order_acquire));
  }

  /** @brief Load the SlotEpochWord for @p slot with acquire ordering. */
  [[nodiscard, gnu::always_inline]] abi::SlotEpochWord GetSlotEpochWord(std::uint32_t slot) const noexcept {
    if (slot >= kMaxSlots) {
      return abi::SlotEpochWord{};
    }
    const std::uint64_t raw = slots_[slot].epoch_word->load(std::memory_order_acquire);
    return abi::SlotEpochWord::from_raw(raw);
  }

  /** @brief Return the ThreadBuffer bound to @p slot, or nullptr if never allocated. */
  [[nodiscard, gnu::always_inline]] ThreadBuffer* GetSlotBuffer(std::uint32_t slot) const noexcept {
    if (slot >= kMaxSlots) {
      return nullptr;
    }
    return slots_[slot].buffer.load(std::memory_order_acquire);
  }

  /** @brief Return the count of thread registration attempts rejected due to 64-slot exhaustion. */
  [[nodiscard]] std::uint64_t RejectedThreads() const noexcept { return rejected_threads_->load(std::memory_order_acquire); }

  /** @brief Set the sticky overflow flag in the SlotEpochWord of @p slot. */
  void MarkSlotOverflowed(std::uint32_t slot) noexcept {
    if (slot >= kMaxSlots) {
      return;
    }
    auto& atom = *slots_[slot].epoch_word;
    std::uint64_t raw = atom.load(std::memory_order_acquire);
    for (;;) {
      const auto word = abi::SlotEpochWord::from_raw(raw);
      if (word.test<^^abi::SlotEpochSpec::overflowed>()) {
        return;
      }
      const auto next = word.with<^^abi::SlotEpochSpec::overflowed>(true);
      if (atom.compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
    }
  }

  /** @brief Reset all 64 slots and counters for deterministic single-process unit tests. */
  void ResetForTesting() noexcept {
    active_mask_->store(0U, std::memory_order_seq_cst);
    rejected_threads_->store(0U, std::memory_order_seq_cst);
    for (std::uint32_t i = 0; i < kMaxSlots; ++i) {
      if (ThreadBuffer* buf = slots_[i].buffer.load(std::memory_order_acquire); buf != nullptr) {
        buf->ResetForEpoch(0U);
      }
      slots_[i].epoch_word->store(0U, std::memory_order_seq_cst);
    }
  }

 private:
  [[nodiscard]] Result<ThreadBuffer*> TryClaimSlot(std::uint32_t slot) noexcept {
    if (slot >= kMaxSlots) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    auto& atom = *slots_[slot].epoch_word;
    std::uint64_t raw = atom.load(std::memory_order_acquire);
    const auto word = abi::SlotEpochWord::from_raw(raw);
    if (word.get<^^abi::SlotEpochSpec::state>() != abi::SlotState::kFree) {
      return std::unexpected(ErrorCode::kTooManyThreads);
    }
    const auto claiming = word.with<^^abi::SlotEpochSpec::state>(abi::SlotState::kDraining);
    if (!atom.compare_exchange_strong(raw, claiming.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return std::unexpected(ErrorCode::kTooManyThreads);
    }
    const std::uint32_t epoch = word.get<^^abi::SlotEpochSpec::epoch>();
    ThreadBuffer* buf = slots_[slot].buffer.load(std::memory_order_acquire);
    if (buf == nullptr) {
      auto created = ThreadBuffer::CreateAllocated(slot, epoch, &atom);
      if (!created) {
        atom.store(word.raw(), std::memory_order_release);
        return std::unexpected(created.error());
      }
      buf = *created;
      slots_[slot].buffer.store(buf, std::memory_order_release);
    } else {
      buf->ResetForEpoch(epoch);
    }
    const auto active =
        word.with<^^abi::SlotEpochSpec::state>(abi::SlotState::kActive).with<^^abi::SlotEpochSpec::overflowed>(false);
    atom.store(active.raw(), std::memory_order_release);
    active_mask_->fetch_or(1ULL << slot, std::memory_order_release);
    return buf;
  }

  std::array<SlotEntry, kMaxSlots> slots_{};
  uarch::Isolated<std::atomic<std::uint64_t>> active_mask_{0U};
  uarch::Isolated<std::atomic<std::uint64_t>> rejected_threads_{0U};
};

inline constinit ThreadRegistry g_registry{};

/** @brief Access the process-wide 64-slot ThreadRegistry singleton. */
[[nodiscard, gnu::always_inline]] inline ThreadRegistry& GlobalRegistry() noexcept { return g_registry; }

inline constinit thread_local bool t_thread_exiting = false;

/** @brief Thread-local RAII guard retiring the calling thread's slot during TLS destruction. */
class ThreadExitGuard final {
 public:
  constexpr ThreadExitGuard() noexcept = default;
  ThreadExitGuard(const ThreadExitGuard&) = delete;
  ThreadExitGuard(ThreadExitGuard&&) = delete;
  ThreadExitGuard& operator=(const ThreadExitGuard&) = delete;
  ThreadExitGuard& operator=(ThreadExitGuard&&) = delete;

  /** @brief Mark the thread as exiting and retire its bound registry slot. */
  ~ThreadExitGuard() noexcept {
    t_thread_exiting = true;
    Retire();
  }

  /** @brief Bind this guard to a newly claimed registry slot. */
  void Bind(ThreadRegistry* registry, std::uint32_t slot) noexcept {
    registry_ = registry;
    slot_ = slot;
    active_ = true;
  }

  /** @brief Retire the bound slot and clear the thread-local buffer pointer. */
  void Retire() noexcept {
    if (active_ && registry_ != nullptr) {
      active_ = false;
      t_buffer = nullptr;
      registry_->RetireSlot(slot_);
    }
  }

 private:
  ThreadRegistry* registry_{nullptr};
  std::uint32_t slot_{0U};
  bool active_{false};
};

inline thread_local ThreadExitGuard t_thread_exit_guard{};

/** @brief Cold-path lazy TLS registration invoked on a thread's first log statement. */
[[nodiscard, gnu::cold, gnu::noinline]] inline ThreadBuffer* RegisterThreadSlow() noexcept {
  if (t_buffer != nullptr) {
    return t_buffer;
  }
  if (uarch::unlikely(t_thread_exiting)) {
    return nullptr;
  }
  std::uint32_t slot = 0U;
  ThreadBuffer* buf = g_registry.RegisterSlot(&slot);
  if (buf != nullptr) {
    t_buffer = buf;
    t_thread_exit_guard.Bind(&g_registry, slot);
  }
  return buf;
}

/** @brief Explicitly retire the calling thread's slot before thread exit. */
inline void RetireCurrentThread() noexcept { t_thread_exit_guard.Retire(); }

/** @brief Eagerly claim and prefault the calling thread's SPSC ring, returning exact ErrorCode on failure. */
[[nodiscard]] inline Result<ThreadBuffer*> PrewarmCurrentThread() noexcept {
  if (t_buffer != nullptr) {
    return t_buffer;
  }
  if (uarch::unlikely(t_thread_exiting)) {
    return std::unexpected(ErrorCode::kShuttingDown);
  }
  std::uint32_t slot = 0U;
  auto res = g_registry.TryRegisterSlot(&slot);
  if (!res) {
    return std::unexpected(res.error());
  }
  t_buffer = *res;
  t_thread_exit_guard.Bind(&g_registry, slot);
  return *res;
}

/** @brief Set the calling thread's diagnostic name (up to 31 bytes), prewarming its slot if needed. */
[[nodiscard]] inline Result<void> TrySetCurrentThreadName(std::string_view name) noexcept {
  if (name.size() > ThreadBuffer::kMaxThreadNameLen) {
    return std::unexpected(ErrorCode::kInvalidArgument);
  }
  auto buf = PrewarmCurrentThread();
  if (!buf) {
    return std::unexpected(buf.error());
  }
  return (*buf)->SetThreadName(name);
}

}  // namespace zerolog::core
