/** @file @brief Exclusive consumer lease and poller ownership state machine. */
#pragma once

#include <atomic>
#include <cstdint>
#include <uarch/uarch.hpp>
#include <utility>
#include <zerolog/abi/contract.hpp>
#include <zerolog/error/zerolog_error.hpp>

namespace zerolog::core {

inline constinit uarch::Isolated<std::atomic<std::uint64_t>> g_consumer_state{0U};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

/** @brief Load the packed 64-bit ConsumerStateWord with the requested memory ordering. */
[[nodiscard, gnu::always_inline]] inline abi::ConsumerStateWord LoadConsumerState(
    std::memory_order order = std::memory_order_acquire) noexcept {
  return abi::ConsumerStateWord::from_raw(g_consumer_state->load(order));
}

/** @brief Permanently poison the consumer lease state and bump its generation counter. */
inline abi::ConsumerStateWord PoisonLease() noexcept {
  std::uint64_t raw = g_consumer_state->load(std::memory_order_acquire);
  for (;;) {
    const auto word = abi::ConsumerStateWord::from_raw(raw);
    const auto next = word.with<^^abi::ConsumerStateSpec::poisoned>(true)
                          .with<^^abi::ConsumerStateSpec::active>(false)
                          .with<^^abi::ConsumerStateSpec::external>(false)
                          .with<^^abi::ConsumerStateSpec::poller_running>(false)
                          .wrapping_add_top_field<^^abi::ConsumerStateSpec::generation>(1U);
    if (g_consumer_state->compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return next;
    }
  }
}

/** @brief Release an active consumer lease matching @p generation. */
inline bool ReleaseLease([[maybe_unused]] std::uint32_t generation) noexcept {
  std::uint64_t raw = g_consumer_state->load(std::memory_order_acquire);
  for (;;) {
    const auto word = abi::ConsumerStateWord::from_raw(raw);
    if (!word.test<^^abi::ConsumerStateSpec::active>() || word.get<^^abi::ConsumerStateSpec::generation>() != generation) {
      return false;
    }
    const auto next = word.with<^^abi::ConsumerStateSpec::active>(false)
                          .with<^^abi::ConsumerStateSpec::external>(false)
                          .with<^^abi::ConsumerStateSpec::poller_running>(false);
    if (g_consumer_state->compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return true;
    }
  }
}

/** @brief Transition an internal poller lease matching @p generation into poller_running=true. */
[[nodiscard]] inline Result<void> CommitPollerStart([[maybe_unused]] std::uint32_t generation) noexcept {
  std::uint64_t raw = g_consumer_state->load(std::memory_order_acquire);
  for (;;) {
    const auto word = abi::ConsumerStateWord::from_raw(raw);
    if (word.test<^^abi::ConsumerStateSpec::poisoned>()) {
      return std::unexpected(ErrorCode::kLeasePoisoned);
    }
    if (!word.test<^^abi::ConsumerStateSpec::active>() || word.test<^^abi::ConsumerStateSpec::external>() ||
        word.get<^^abi::ConsumerStateSpec::generation>() != generation) {
      return std::unexpected(ErrorCode::kConsumerBusy);
    }
    const auto next = word.with<^^abi::ConsumerStateSpec::poller_running>(true);
    if (g_consumer_state->compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return {};
    }
  }
}

/** @brief Move-only RAII token representing exclusive consumer or poller ownership. */
class ConsumerLease final {
 public:
  constexpr ConsumerLease() noexcept = default;
  /** @brief Construct an active lease for @p generation. */
  constexpr ConsumerLease(std::uint32_t generation, bool external) noexcept
      : generation_(generation), active_(true), external_(external) {}

  ConsumerLease(const ConsumerLease&) = delete ("ConsumerLease is move-only exclusive ownership");
  ConsumerLease& operator=(const ConsumerLease&) = delete ("ConsumerLease is move-only exclusive ownership");

  /** @brief Transfer lease ownership from @p other. */
  ConsumerLease(ConsumerLease&& other) noexcept
      : generation_(std::exchange(other.generation_, 0U)),
        active_(std::exchange(other.active_, false)),
        external_(std::exchange(other.external_, false)) {}

  /** @brief Release any held lease and transfer ownership from @p other. */
  ConsumerLease& operator=(ConsumerLease&& other) noexcept {
    if (this != &other) {
      (void)Release();
      generation_ = std::exchange(other.generation_, 0U);
      active_ = std::exchange(other.active_, false);
      external_ = std::exchange(other.external_, false);
    }
    return *this;
  }

  /** @brief Automatically release the lease on scope exit if still held. */
  ~ConsumerLease() noexcept { (void)Release(); }

  /** @brief Return true if this token currently holds an active lease. */
  [[nodiscard, gnu::always_inline]] constexpr bool Valid() const noexcept { return active_; }
  /** @brief Return true if this lease was acquired for an external Sans-IO LogConsumer. */
  [[nodiscard, gnu::always_inline]] constexpr bool IsExternal() const noexcept { return external_; }
  /** @brief Return the 32-bit generation counter associated with this lease. */
  [[nodiscard, gnu::always_inline]] constexpr std::uint32_t Generation() const noexcept { return generation_; }

  /** @brief Explicitly release the held lease back to the idle state. */
  bool Release() noexcept {
    if (!active_) {
      return false;
    }
    active_ = false;
    external_ = false;
    return ReleaseLease(generation_);
  }

  /** @brief Permanently poison the global consumer state and invalidate this token. */
  abi::ConsumerStateWord Poison() noexcept {
    active_ = false;
    external_ = false;
    return PoisonLease();
  }

 private:
  std::uint32_t generation_{0U};
  bool active_{false};
  bool external_{false};
};

/** @brief Acquire an exclusive external consumer lease or return kLeaseHeld / kConsumerBusy / kLeasePoisoned. */
[[nodiscard]] inline Result<ConsumerLease> TryAcquireExternalLease() noexcept {
  std::uint64_t raw = g_consumer_state->load(std::memory_order_acquire);
  for (;;) {
    const auto word = abi::ConsumerStateWord::from_raw(raw);
    if (word.test<^^abi::ConsumerStateSpec::poisoned>()) {
      return std::unexpected(ErrorCode::kLeasePoisoned);
    }
    if (word.test<^^abi::ConsumerStateSpec::active>() || word.test<^^abi::ConsumerStateSpec::poller_running>()) {
      return std::unexpected(word.test<^^abi::ConsumerStateSpec::external>() ? ErrorCode::kLeaseHeld
                                                                             : ErrorCode::kConsumerBusy);
    }
    const auto next = word.with<^^abi::ConsumerStateSpec::active>(true)
                          .with<^^abi::ConsumerStateSpec::external>(true)
                          .with<^^abi::ConsumerStateSpec::poller_running>(false)
                          .wrapping_add_top_field<^^abi::ConsumerStateSpec::generation>(1U);
    if (g_consumer_state->compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return ConsumerLease{next.get<^^abi::ConsumerStateSpec::generation>(), true};
    }
  }
}

/** @brief Acquire an internal poller startup lease or return kLeaseHeld / kConsumerBusy / kLeasePoisoned. */
[[nodiscard]] inline Result<ConsumerLease> TryStartPoller() noexcept {
  std::uint64_t raw = g_consumer_state->load(std::memory_order_acquire);
  for (;;) {
    const auto word = abi::ConsumerStateWord::from_raw(raw);
    if (word.test<^^abi::ConsumerStateSpec::poisoned>()) {
      return std::unexpected(ErrorCode::kLeasePoisoned);
    }
    if (word.test<^^abi::ConsumerStateSpec::external>()) {
      return std::unexpected(ErrorCode::kLeaseHeld);
    }
    if (word.test<^^abi::ConsumerStateSpec::active>() || word.test<^^abi::ConsumerStateSpec::poller_running>()) {
      return std::unexpected(ErrorCode::kConsumerBusy);
    }
    const auto next = word.with<^^abi::ConsumerStateSpec::active>(true)
                          .with<^^abi::ConsumerStateSpec::external>(false)
                          .with<^^abi::ConsumerStateSpec::poller_running>(false)
                          .wrapping_add_top_field<^^abi::ConsumerStateSpec::generation>(1U);
    if (g_consumer_state->compare_exchange_weak(raw, next.raw(), std::memory_order_acq_rel, std::memory_order_acquire)) {
      return ConsumerLease{next.get<^^abi::ConsumerStateSpec::generation>(), false};
    }
  }
}

/** @brief Commit a poller startup lease to poller_running=true. */
[[nodiscard]] inline Result<void> CommitPollerStart(const ConsumerLease& lease) noexcept {
  if (!lease.Valid() || lease.IsExternal()) {
    return std::unexpected(ErrorCode::kConsumerBusy);
  }
  return CommitPollerStart(lease.Generation());
}

/** @brief Reset the global ConsumerStateWord for deterministic unit tests. */
inline void ResetLeaseStateForTesting() noexcept { g_consumer_state->store(0U, std::memory_order_seq_cst); }

}  // namespace zerolog::core
