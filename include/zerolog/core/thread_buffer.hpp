/** @file @brief Per-thread SPSC buffer, queue-full policy, reentrancy guards and seqlock thread naming. */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>
#include <thread>
#include <uarch/uarch.hpp>
#include <utility>
#include <zerolog/abi/contract.hpp>
#include <zerolog/abi/record_layout.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/spsc_var_queue.hpp>
#include <zerolog/error/zerolog_error.hpp>

namespace zerolog::core {

inline constexpr std::uint8_t kReentrancyNone = 0U;
inline constexpr std::uint8_t kReentrancyInConsumer = 1U << 0;
inline constexpr std::uint8_t kReentrancyInQueueFullCallback = 1U << 1;
inline constexpr std::uint8_t kReentrancyInProducerReservation = 1U << 2;

inline constinit thread_local std::uint8_t t_reentrancy_flags = kReentrancyNone;

/** @brief Lexical RAII guard setting a thread-local reentrancy flag for the duration of a scope. */
class ScopedReentrancyFlag final {
 public:
  /** @brief Enter the guarded scope and set @p flag in thread-local reentrancy state. */
  explicit ScopedReentrancyFlag(std::uint8_t flag) noexcept
      : prev_(t_reentrancy_flags), entered_((t_reentrancy_flags & flag) == 0U) {
    t_reentrancy_flags = static_cast<std::uint8_t>(t_reentrancy_flags | flag);
  }
  ScopedReentrancyFlag(const ScopedReentrancyFlag&) = delete ("ScopedReentrancyFlag is lexical and non-copyable");
  ScopedReentrancyFlag(ScopedReentrancyFlag&&) = delete ("ScopedReentrancyFlag is lexical and non-movable");
  ScopedReentrancyFlag& operator=(const ScopedReentrancyFlag&) = delete;
  ScopedReentrancyFlag& operator=(ScopedReentrancyFlag&&) = delete;

  /** @brief Restore the previous thread-local reentrancy flags. */
  ~ScopedReentrancyFlag() noexcept { t_reentrancy_flags = prev_; }

  /** @brief Return true if @p flag was not already set prior to this scope. */
  [[nodiscard, gnu::always_inline]] bool Entered() const noexcept { return entered_; }

 private:
  std::uint8_t prev_;
  bool entered_;
};

/** @brief Return true if @p flag is currently active on the calling thread. */
[[nodiscard, gnu::always_inline]] inline bool HasReentrancyFlag(std::uint8_t flag) noexcept {
  return (t_reentrancy_flags & flag) != 0U;
}

/** @brief Preserve the thread-buffer TLS reentrancy word rather than introducing an independent guard domain. */
using ReentrancyGuard = ScopedReentrancyFlag;
inline constexpr auto kInConsumer = kReentrancyInConsumer;
inline constexpr auto kInQueueFullCallback = kReentrancyInQueueFullCallback;
inline constexpr auto kInProducerReservation = kReentrancyInProducerReservation;
/** @brief Consumer work cannot enter through encoding, callbacks or another consumer step. */
[[nodiscard]] inline bool ConsumerReentry() noexcept {
  return HasReentrancyFlag(kInConsumer | kInQueueFullCallback | kInProducerReservation);
}

/** @brief Callback invoked once when a producer thread encounters a full SPSC ring. */
using QueueFullCallback = void (*)(std::uint32_t slot, std::uint32_t requested_blocks, abi::Level level) noexcept;

inline constinit uarch::Isolated<std::atomic<abi::QueueFullPolicy>> g_queue_full_policy{abi::QueueFullPolicy::kDrop};
inline constinit uarch::Isolated<std::atomic<QueueFullCallback>> g_queue_full_callback{nullptr};

/** @brief Set the global policy applied when a producer ring is full. */
inline void SetQueueFullPolicy(abi::QueueFullPolicy policy) noexcept {
  g_queue_full_policy->store(policy, std::memory_order_release);
}

/** @brief Load the current global queue-full policy. */
[[nodiscard, gnu::always_inline]] inline abi::QueueFullPolicy GetQueueFullPolicy() noexcept {
  return g_queue_full_policy->load(std::memory_order_acquire);
}

/** @brief Register or clear the global callback invoked when a producer ring overflows. */
inline void SetQueueFullCallback(QueueFullCallback callback) noexcept {
  g_queue_full_callback->store(callback, std::memory_order_release);
}

/** @brief Load the currently registered queue-full callback. */
[[nodiscard, gnu::always_inline]] inline QueueFullCallback GetQueueFullCallback() noexcept {
  return g_queue_full_callback->load(std::memory_order_acquire);
}

/** @brief Stack-allocated snapshot of a thread name read via lock-free seqlock. */
struct ThreadName {
  std::array<char, 32> data{};
  std::uint8_t size{0};

  /** @brief Return a string_view over the valid characters in this snapshot. */
  [[nodiscard]] constexpr std::string_view View() const noexcept { return {data.data(), size}; }
};

/** @brief Per-thread producer buffer pinned to one of the 64 registry slots. */
class ThreadBuffer final {
 public:
  static constexpr std::size_t kMaxThreadNameLen = 31;

  SpscVarQueue queue;
  std::uint32_t slot_index{0};
  std::uint32_t slot_epoch{0};
  uarch::Isolated<std::atomic<std::uint64_t>> dropped_records{0};
  uarch::Isolated<std::atomic<std::uint64_t>> blocked_events{0};

  ThreadBuffer(const ThreadBuffer&) = delete ("ThreadBuffer is pinned to a registry slot");
  ThreadBuffer(ThreadBuffer&&) = delete ("ThreadBuffer is pinned to a registry slot");
  ThreadBuffer& operator=(const ThreadBuffer&) = delete;
  ThreadBuffer& operator=(ThreadBuffer&&) = delete;

  /** @brief Allocate a new ThreadBuffer and its 2 MiB THP ring, returning an exact ErrorCode on failure. */
  [[nodiscard]] static Result<ThreadBuffer*> CreateAllocated(std::uint32_t slot, std::uint32_t epoch,
                                                             std::atomic<std::uint64_t>* epoch_word = nullptr) noexcept {
    auto q = SpscVarQueue::Create();
    if (!q) {
      return std::unexpected(q.error());
    }
    auto* buf = new (std::nothrow) ThreadBuffer(std::move(*q), slot, epoch, epoch_word);
    if (buf == nullptr) {
      return std::unexpected(ErrorCode::kWriteFailed);
    }
    return buf;
  }

  /** @brief Fast-path producer reservation forwarding directly to the SPSC ring. */
  [[nodiscard, gnu::always_inline]] std::byte* TryReserve(std::uint32_t blocks) noexcept { return queue.TryReserve(blocks); }

  /** @brief Roll back an uncommitted producer reservation and scrub its blocks to zero. */
  [[gnu::cold, gnu::noinline]] void CancelReservation(std::byte* record, std::uint32_t blocks) noexcept {
    queue.CancelReservation(record, blocks);
  }

  /** @brief Slow-path reservation applying reentrancy checks and queue-full policy, returning exact ErrorCode. */
  [[nodiscard, gnu::cold, gnu::noinline]] Result<std::byte*> TryReserveSlow(std::uint32_t blocks,
                                                                            abi::Level level = abi::Level::kInfo) noexcept {
    if (blocks == 0U || blocks > abi::kMaxRecordBlocks) {
      return std::unexpected(ErrorCode::kRecordTooLarge);
    }
    if (HasReentrancyFlag(kReentrancyInProducerReservation)) {
      dropped_records->fetch_add(1U, std::memory_order_relaxed);
      return std::unexpected(ErrorCode::kConsumerBusy);
    }
    if (auto res = queue.TryReserveSlow(blocks); res.has_value()) {
      return *res;
    }

    if (const auto cb = GetQueueFullCallback(); cb != nullptr && !HasReentrancyFlag(kReentrancyInQueueFullCallback)) {
      ScopedReentrancyFlag cb_guard{kReentrancyInQueueFullCallback};
      cb(slot_index, blocks, level);
    }

    const auto policy = GetQueueFullPolicy();
    if (policy == abi::QueueFullPolicy::kDrop || HasReentrancyFlag(kReentrancyInConsumer) ||
        HasReentrancyFlag(kReentrancyInQueueFullCallback)) {
      dropped_records->fetch_add(1U, std::memory_order_relaxed);
      MarkOverflowed();
      return std::unexpected(ErrorCode::kQueueFull);
    }

    blocked_events->fetch_add(1U, std::memory_order_relaxed);
    RingDoorbellUrgentCold();
    for (std::uint32_t spins = 0;; ++spins) {
      if (auto res = queue.TryReserveSlow(blocks); res.has_value()) {
        return *res;
      }
      if ((spins & 63U) == 63U) {
        RingDoorbellUrgentCold();
        std::this_thread::yield();
      } else {
        asm volatile("pause" ::: "memory");
      }
    }
  }

  /** @brief Slow-path reservation returning nullptr on failure for hot-to-cold call sites. */
  [[nodiscard, gnu::cold, gnu::noinline]] std::byte* ReserveSlow(std::uint32_t blocks,
                                                                 abi::Level level = abi::Level::kInfo) noexcept {
    auto res = TryReserveSlow(blocks, level);
    return res ? *res : nullptr;
  }

  /** @brief Consume and clear the pending 50% watermark notification flag. */
  [[nodiscard, gnu::always_inline]] bool ConsumeWatermarkNotify() noexcept { return queue.ConsumeNotifyAfterPublish(); }

  /** @brief Release-publish a record header and ring the consumer doorbell if watermark or flush level requires. */
  [[gnu::always_inline]] void PublishAndNotify(std::byte* record, abi::RecordHeaderWord header, abi::Level level) noexcept {
    SpscVarQueue::Publish(record, header);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const bool watermark_notify = ConsumeWatermarkNotify();
    const auto fresh = LoadLevelGate(std::memory_order_relaxed);
    if (uarch::unlikely(watermark_notify)) {
      uarch::cold_call<&RingDoorbellUrgentCold>();
    } else if (uarch::unlikely(level >= fresh.get<^^abi::LevelGateSpec::ring_level>())) {
      uarch::cold_call<&RingDoorbellCold>();
    }
  }

  /** @brief Publish a new thread name (up to 31 bytes) via seqlock, or return kInvalidArgument if too long. */
  [[nodiscard]] Result<void> SetThreadName(std::string_view name) noexcept {
    if (name.size() > kMaxThreadNameLen) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    std::array<char, 32> raw_bytes{};
    if (!name.empty()) {
      std::memcpy(raw_bytes.data(), name.data(), name.size());
    }
    raw_bytes[31] = static_cast<char>(name.size());
    std::array<std::uint64_t, 4> words{};
    std::memcpy(words.data(), raw_bytes.data(), sizeof(words));

    name_seq_.fetch_add(1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < 4U; ++i) {
      name_words_[i].store(words[i], std::memory_order_relaxed);
    }
    name_seq_.fetch_add(1U, std::memory_order_release);
    return {};
  }

  /** @brief Read a tear-free ThreadName snapshot by value using the seqlock protocol. */
  [[nodiscard]] ThreadName ReadThreadName() const noexcept {
    for (;;) {
      const std::uint32_t seq0 = name_seq_.load(std::memory_order_acquire);
      if ((seq0 & 1U) != 0U) {
        asm volatile("pause" ::: "memory");
        continue;
      }
      std::array<std::uint64_t, 4> words{};
      for (std::size_t i = 0; i < 4U; ++i) {
        words[i] = name_words_[i].load(std::memory_order_relaxed);
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      const std::uint32_t seq1 = name_seq_.load(std::memory_order_relaxed);
      if (seq0 == seq1) {
        ThreadName out{};
        std::memcpy(out.data.data(), words.data(), sizeof(words));
        const auto len = static_cast<std::uint8_t>(out.data[31]);
        out.size = len <= kMaxThreadNameLen ? len : 0U;
        out.data[out.size] = '\0';
        return out;
      }
    }
  }

  /** @brief Return a string_view backed by a thread-local snapshot of ReadThreadName(). */
  [[nodiscard]] std::string_view GetThreadName() const noexcept {
    thread_local ThreadName t_name_scratch{};
    t_name_scratch = ReadThreadName();
    return t_name_scratch.View();
  }

  /** @brief Reset this ThreadBuffer for reuse under a new slot epoch after draining. */
  void ResetForEpoch(std::uint32_t epoch) noexcept {
    queue.ResetForReuse();
    slot_epoch = epoch;
    dropped_records->store(0U, std::memory_order_relaxed);
    blocked_events->store(0U, std::memory_order_relaxed);
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    (void)SetThreadName("");
  }

  /** @brief Return total records dropped on this slot in the current epoch. */
  [[nodiscard]] std::uint64_t DroppedRecords() const noexcept { return dropped_records->load(std::memory_order_acquire); }

  /** @brief Return total times this producer blocked waiting for ring space in the current epoch. */
  [[nodiscard]] std::uint64_t BlockedEvents() const noexcept { return blocked_events->load(std::memory_order_acquire); }

 private:
  ThreadBuffer(SpscVarQueue q, std::uint32_t slot, std::uint32_t epoch, std::atomic<std::uint64_t>* epoch_word) noexcept
      : queue(std::move(q)), slot_index(slot), slot_epoch(epoch), slot_epoch_word_(epoch_word) {}

  void MarkOverflowed() noexcept {
    if (slot_epoch_word_ == nullptr) {
      return;
    }
    std::uint64_t raw = slot_epoch_word_->load(std::memory_order_acquire);
    for (;;) {
      const auto word = abi::SlotEpochWord::from_raw(raw);
      if (word.test<^^abi::SlotEpochSpec::overflowed>()) {
        return;
      }
      const auto next = word.with<^^abi::SlotEpochSpec::overflowed>(true);
      if (slot_epoch_word_->compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
    }
  }

  std::atomic<std::uint64_t>* slot_epoch_word_{nullptr};
  std::atomic<std::uint32_t> name_seq_{0U};
  std::array<std::atomic<std::uint64_t>, 4> name_words_{};
};

inline constinit thread_local ThreadBuffer* t_buffer = nullptr;

}  // namespace zerolog::core

#include <zerolog/core/registry.hpp>
