/** @file @brief Wake and idle-timer transitions. The timer object also tracks one `TIMEOUT_UPDATE`. */
#pragma once

#include <cerrno>
#include <cstdint>
#include <uarch/uarch.hpp>
#include <zerolog/abi/contract.hpp>
#include <zerolog/error/zerolog_error.hpp>

namespace zerolog::sink {

namespace detail {

[[nodiscard, gnu::always_inline]] constexpr abi::LifecycleState LifecycleOf(
    [[maybe_unused]] abi::SinkFlagsWord flags) noexcept {
  return flags.get<^^abi::SinkFlagsSpec::lifecycle>();
}
[[nodiscard, gnu::always_inline]] constexpr abi::WakeState WakeOf([[maybe_unused]] abi::SinkFlagsWord flags) noexcept {
  return flags.get<^^abi::SinkFlagsSpec::wake>();
}
[[nodiscard, gnu::always_inline]] constexpr abi::TimerState TimerOf([[maybe_unused]] abi::SinkFlagsWord flags) noexcept {
  return flags.get<^^abi::SinkFlagsSpec::timer>();
}
[[nodiscard, gnu::always_inline]] constexpr abi::WriteRotateState IoOf([[maybe_unused]] abi::SinkFlagsWord flags) noexcept {
  return flags.get<^^abi::SinkFlagsSpec::io>();
}
[[nodiscard, gnu::always_inline]] constexpr bool WriteRetryOf([[maybe_unused]] abi::SinkFlagsWord flags) noexcept {
  return flags.test<^^abi::SinkFlagsSpec::write_retry>();
}
[[nodiscard, gnu::always_inline]] constexpr bool RotationRetryOf([[maybe_unused]] abi::SinkFlagsWord flags) noexcept {
  return flags.test<^^abi::SinkFlagsSpec::rotation_retry>();
}
[[nodiscard, gnu::always_inline]] constexpr abi::SinkFlagsWord WithLifecycle(
    [[maybe_unused]] abi::SinkFlagsWord flags, [[maybe_unused]] abi::LifecycleState state) noexcept {
  return flags.with<^^abi::SinkFlagsSpec::lifecycle>(state);
}
[[nodiscard, gnu::always_inline]] constexpr abi::SinkFlagsWord WithTimer([[maybe_unused]] abi::SinkFlagsWord flags,
                                                                         [[maybe_unused]] abi::TimerState state) noexcept {
  return flags.with<^^abi::SinkFlagsSpec::timer>(state);
}
[[nodiscard, gnu::always_inline]] constexpr abi::SinkFlagsWord RunningReadyFlags() noexcept {
  return abi::SinkFlagsWord{}
      .with<^^abi::SinkFlagsSpec::lifecycle>(abi::LifecycleState::kRunning)
      .with<^^abi::SinkFlagsSpec::io>(abi::WriteRotateState::kReady);
}
[[nodiscard, gnu::always_inline]] constexpr bool DrainMorePending([[maybe_unused]] abi::DrainFlagsWord flags) noexcept {
  return flags.test<^^abi::DrainFlagsSpec::more_pending>() || flags.test<^^abi::DrainFlagsSpec::budget_exhausted>();
}
[[nodiscard, gnu::always_inline]] constexpr bool DrainBackpressure([[maybe_unused]] abi::DrainFlagsWord flags) noexcept {
  return flags.test<^^abi::DrainFlagsSpec::backpressure>();
}
[[nodiscard, gnu::always_inline]] constexpr bool DrainHasMore(abi::DrainFlagsWord flags) noexcept {
  return DrainMorePending(flags) || DrainBackpressure(flags);
}

}  // namespace detail

/**
 * @brief Futex-wait state for the doorbell.
 * Completions always leave the in-flight state. An unexpected completion is returned to the caller.
 */
struct WakeFsm {
  /** @brief `kIdle` -> `kArmed`. A sink that is stopping or already waiting returns an error. */
  [[nodiscard]] static Result<void> TryArm(zerolog::abi::SinkFlagsWord& flags) noexcept {
    if (Stopped(flags)) return std::unexpected(ErrorCode::kShuttingDown);
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::wake>() != zerolog::abi::WakeState::kIdle) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::wake>(zerolog::abi::WakeState::kArmed);
    return {};
  }

  /**
   * @brief Consume one futex-wait completion.
   * @return `true` when the caller should arm again. `false` means do not arm.
   * `kCanceling` stays canceled for `0`, `-EAGAIN`, and `-ECANCELED`. `-ECANCELED` while armed is legal
   * only if the sink is stopping.
   */
  [[nodiscard]] static Result<bool> OnWakeCqe(zerolog::abi::SinkFlagsWord& flags, std::int32_t cqe_res) noexcept {
    const auto wake = flags.get<^^zerolog::abi::SinkFlagsSpec::wake>();
    if (wake == zerolog::abi::WakeState::kIdle) return std::unexpected(ErrorCode::kInvalidArgument);
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::wake>(zerolog::abi::WakeState::kIdle);
    // The cancel already won. A racing wake must not ask the caller to arm again.
    if (wake == zerolog::abi::WakeState::kCanceling) {
      if (cqe_res == 0 || cqe_res == -EAGAIN || cqe_res == -ECANCELED) return false;
      return std::unexpected(ErrorCode::kProactorUnavailable);
    }
    if (cqe_res == 0 || cqe_res == -EAGAIN) return true;
    if (cqe_res == -ECANCELED) {
      if (Stopped(flags)) return false;
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    return std::unexpected(ErrorCode::kProactorUnavailable);
  }

  /**
   * @brief `kArmed` -> `kCanceling`.
   * @return `true` when the caller must submit a cancel. `false` when no wait is armed or a cancel is already posted.
   */
  [[nodiscard]] static bool RequestCancel(zerolog::abi::SinkFlagsWord& flags) noexcept {
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::wake>() != zerolog::abi::WakeState::kArmed) return false;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::wake>(zerolog::abi::WakeState::kCanceling);
    return true;
  }

  /** @brief The cancel SQE was never submitted (`engine.Contains(token) == false`), so no cancel CQE will arrive. */
  static void ReconcileSyncCancel(zerolog::abi::SinkFlagsWord& flags) noexcept {
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::wake>() != zerolog::abi::WakeState::kCanceling) return;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::wake>(zerolog::abi::WakeState::kIdle);
  }

 private:
  [[nodiscard]] static bool Stopped(const zerolog::abi::SinkFlagsWord& flags) noexcept {
    const auto life = flags.get<^^zerolog::abi::SinkFlagsSpec::lifecycle>();
    return life == zerolog::abi::LifecycleState::kStopping || life == zerolog::abi::LifecycleState::kDetached ||
           life == zerolog::abi::LifecycleState::kClosed;
  }
};

/**
 * @brief Idle-timeout state plus one outstanding `TIMEOUT_UPDATE`.
 * The 2-bit `TimerState` is the primary timeout only. `update_in_flight_` remembers the update SQE after
 * that primary completion moves the word to `kIdle` or a later `TryArm` moves it to `kArmed`.
 * An update changes the deadline; it does not cancel the primary timeout.
 */
class TimerFsm {
 public:
  /** @brief `kIdle` -> `kArmed`. An outstanding update does not block the next primary timeout. */
  [[nodiscard]] static Result<void> TryArm(zerolog::abi::SinkFlagsWord& flags) noexcept {
    if (Stopped(flags)) return std::unexpected(ErrorCode::kShuttingDown);
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::timer>() != zerolog::abi::TimerState::kIdle) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::timer>(zerolog::abi::TimerState::kArmed);
    return {};
  }

  /**
   * @brief `kArmed` -> `kUpdating` and post one `TIMEOUT_UPDATE`.
   * A second update while the first completion is outstanding is `kConsumerBusy`.
   */
  [[nodiscard]] Result<void> RequestImmediateUpdate(zerolog::abi::SinkFlagsWord& flags) noexcept {
    if (Stopped(flags)) return std::unexpected(ErrorCode::kShuttingDown);
    if (update_in_flight_ != 0) return std::unexpected(ErrorCode::kConsumerBusy);
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::timer>() != zerolog::abi::TimerState::kArmed) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    update_in_flight_ = 1;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::timer>(zerolog::abi::TimerState::kUpdating);
    return {};
  }

  /**
   * @brief Consume the primary timeout completion and move `timer` to `kIdle`.
   * Does not retire an outstanding `TIMEOUT_UPDATE`; that SQE still has its own completion.
   * @return `true` when `-ETIME` fired the deadline. `false` is a legal cancel.
   */
  [[nodiscard]] static Result<bool> OnTimerCqe(zerolog::abi::SinkFlagsWord& flags, std::int32_t cqe_res) noexcept {
    const auto timer = flags.get<^^zerolog::abi::SinkFlagsSpec::timer>();
    if (timer == zerolog::abi::TimerState::kIdle) return std::unexpected(ErrorCode::kInvalidArgument);
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::timer>(zerolog::abi::TimerState::kIdle);
    if (cqe_res == -ETIME) return true;
    if (cqe_res == -ECANCELED) {
      if (timer == zerolog::abi::TimerState::kCanceling || Stopped(flags)) return false;
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    return std::unexpected(ErrorCode::kProactorUnavailable);
  }

  /**
   * @brief Consume the one outstanding `TIMEOUT_UPDATE` completion.
   * `0`, `-ENOENT`, `-EALREADY`, and `-ECANCELED` are accepted. Any other negative result is `kWriteFailed`.
   * `kUpdating` returns to `kArmed` because the primary timeout is still posted. Any other timer state is left as it is.
   */
  [[nodiscard]] Result<void> OnUpdateCqe(zerolog::abi::SinkFlagsWord& flags, std::int32_t cqe_res) noexcept {
    if (update_in_flight_ == 0) return std::unexpected(ErrorCode::kInvalidArgument);
    update_in_flight_ = 0;
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::timer>() == zerolog::abi::TimerState::kUpdating) {
      flags = flags.with<^^zerolog::abi::SinkFlagsSpec::timer>(zerolog::abi::TimerState::kArmed);
    }
    if (cqe_res == 0 || cqe_res == -ENOENT || cqe_res == -EALREADY || cqe_res == -ECANCELED) return {};
    if (cqe_res < 0) return std::unexpected(ErrorCode::kWriteFailed);
    return std::unexpected(ErrorCode::kInvalidArgument);
  }

  /**
   * @brief `kArmed` -> `kCanceling`.
   * @return `true` when the caller must submit `TIMEOUT_REMOVE`. An in-flight update is left untouched.
   */
  [[nodiscard]] static bool RequestCancel(zerolog::abi::SinkFlagsWord& flags) noexcept {
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::timer>() != zerolog::abi::TimerState::kArmed) return false;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::timer>(zerolog::abi::TimerState::kCanceling);
    return true;
  }

  /** @brief The remove SQE was never submitted, so no cancel CQE will arrive. */
  static void ReconcileSyncCancel(zerolog::abi::SinkFlagsWord& flags) noexcept {
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::timer>() != zerolog::abi::TimerState::kCanceling) return;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::timer>(zerolog::abi::TimerState::kIdle);
  }

  /** @brief The update SQE was never submitted. `kUpdating` falls back to the still-posted primary timeout. */
  void ReconcileSyncUpdateCancel(zerolog::abi::SinkFlagsWord& flags) noexcept {
    update_in_flight_ = 0;
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::timer>() == zerolog::abi::TimerState::kUpdating) {
      flags = flags.with<^^zerolog::abi::SinkFlagsSpec::timer>(zerolog::abi::TimerState::kArmed);
    }
  }

  /** @brief True while the primary timeout is not idle or a `TIMEOUT_UPDATE` completion is still due. */
  [[nodiscard]] bool InFlight(zerolog::abi::SinkFlagsWord flags) const noexcept {
    return flags.get<^^zerolog::abi::SinkFlagsSpec::timer>() != zerolog::abi::TimerState::kIdle || update_in_flight_ != 0;
  }

  /** @brief True while the `TIMEOUT_UPDATE` completion has not arrived. */
  [[nodiscard]] bool UpdateInFlight() const noexcept { return update_in_flight_ != 0; }

 private:
  [[nodiscard]] static bool Stopped(const zerolog::abi::SinkFlagsWord& flags) noexcept {
    const auto life = flags.get<^^zerolog::abi::SinkFlagsSpec::lifecycle>();
    return life == zerolog::abi::LifecycleState::kStopping || life == zerolog::abi::LifecycleState::kDetached ||
           life == zerolog::abi::LifecycleState::kClosed;
  }

  std::uint8_t update_in_flight_{};  ///< One posted `TIMEOUT_UPDATE` whose CQE has not been consumed.
};

static_assert(uarch::NoPadding<TimerFsm>);

}  // namespace zerolog::sink

namespace zerolog {
using sink::TimerFsm;
using sink::WakeFsm;
}  // namespace zerolog
