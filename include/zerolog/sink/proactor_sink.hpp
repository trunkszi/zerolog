/** @file @brief Asynchronous `io_uring` log sink subsystem coordinating wake, timer, and write/rotate FSMs. */
#pragma once

#include <time.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <uarch/uarch.hpp>
#include <uring/abi/time.hpp>
#include <uring/abi/uapi_io_uring.hpp>
#include <uring/caps/required_caps.hpp>
#include <uring/core/completion.hpp>
#include <uring/core/proactor.hpp>
#include <uring/core/subsystem.hpp>
#include <uring/core/user_data_token.hpp>
#include <uring/ops/core.hpp>
#include <uring/ops/fs.hpp>
#include <uring/ops/futex.hpp>
#include <uring/sys/syscall.hpp>
#include <uring/sys/unique_fd.hpp>
#include <utility>
#include <zerolog/abi/contract.hpp>
#include <zerolog/core/consumer.hpp>
#include <zerolog/core/consumer_core.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/format/output_buffer.hpp>
#include <zerolog/sink/rotation.hpp>
#include <zerolog/sink/wake_timer_fsm.hpp>
#include <zerolog/sink/write_rotate_fsm.hpp>
#include <zerolog/sys/tsc_clock.hpp>

namespace zerolog::sink {

using SubsystemId = std::uint8_t;

/** @brief Tuning parameters for `ProactorLogSink` and `FutexFallbackSink`. */
struct SinkConfig {
  abi::DrainBudget drain_budget{4096U, 1024U * 1024U};  ///< Per-turn record and byte drain ceiling.
  std::uint64_t idle_timeout_ns{1'000'000ULL};          ///< Periodic flush poll interval in nanoseconds.
  std::uint8_t manage_deep_park{0U};  ///< 0 = AdaptiveTimer (default); N > 0 = enter DeepPark after N empty idle polls.
  std::uint8_t reserved[7]{};         ///< Keeps `SinkConfig` padding-free.
};
static_assert(sizeof(SinkConfig) == 24 && uarch::NoPadding<SinkConfig>);

namespace detail {

/** @brief Convert milliseconds in `0..1000` to `KernelTimespec` without division instructions. */
[[nodiscard, gnu::always_inline]] constexpr uring::abi::KernelTimespec BackoffTimespec(std::uint32_t backoff_ms) noexcept {
  if (backoff_ms >= 1000U) {
    return uring::abi::KernelTimespec{.tv_sec = 1, .tv_nsec = 0};
  }
  return uring::abi::KernelTimespec{.tv_sec = 0, .tv_nsec = static_cast<std::int64_t>(backoff_ms) * 1'000'000LL};
}

/** @brief Convert nanoseconds to `KernelTimespec` at configuration time without hardware division in hot paths. */
[[nodiscard, gnu::noinline]] inline uring::abi::KernelTimespec ConfiguredTimespec(std::uint64_t ns) noexcept {
  constexpr std::uint64_t ns_per_sec = 1'000'000'000ULL;
  if (ns < ns_per_sec) {
    return uring::abi::KernelTimespec{.tv_sec = 0, .tv_nsec = static_cast<std::int64_t>(ns)};
  }
  const auto sec = static_cast<std::uint64_t>((static_cast<__uint128_t>(ns) * 0x89705f4136b4a597ULL) >> 93U);
  const auto rem = ns - sec * ns_per_sec;
  const auto adjust = static_cast<std::uint64_t>(rem >= ns_per_sec);
  return uring::abi::KernelTimespec{
      .tv_sec = static_cast<std::int64_t>(sec + adjust),
      .tv_nsec = static_cast<std::int64_t>(rem - adjust * ns_per_sec),
  };
}

/** @brief Read `CLOCK_MONOTONIC` in nanoseconds. */
[[nodiscard, gnu::noinline]] inline std::uint64_t MonotonicNowNs() noexcept {
  timespec ts{};
  if (uarch::unlikely(::clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 || ts.tv_nsec < 0)) return 0ULL;
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace detail

/**
 * @brief Mode A `io_uring` log sink satisfying `uring::plugins::Subsystem`.
 *
 * Coordinates `WakeFsm` (`FUTEX_WAIT` on `core::g_doorbell`), `TimerFsm` (`TIMEOUT` / `TIMEOUT_UPDATE`),
 * and `WriteRotateFsm` (strictly single in-flight `WRITE` plus multi-step `UNLINKAT` / `RENAMEAT` / `OPENAT` /
 * `CLOSE` rotation sharing the resume cursor on `Rotation`).
 */
template <class Engine = uring::Proactor, SubsystemId Id = SubsystemId{1}, class RotationType = Rotation>
class ProactorLogSink final {
  static_assert(Id != uring::kInternalSubsystemId && Id != uring::kMailboxSubsystemId,
                "ProactorLogSink requires a user SubsystemId");

 public:
  static constexpr std::uint8_t kSubsystemId = Id;

  /** @brief Declare the exact `io_uring` opcode and futex2 capabilities required by `ProactorLogSink`. */
  [[nodiscard]] static consteval uring::caps::RequiredCaps RequiredCaps() noexcept {
    using namespace uring::abi;
    return uring::caps::RequiredCaps::For<kOpFutexWait, kOpTimeout, kOpTimeoutRemove, kOpWrite, kOpOpenat, kOpRenameat,
                                          kOpUnlinkat, kOpClose, kOpFsync>();
  }

  /** @brief Construct a sink bound to an existing `ConsumerCore` and opened `Rotation`. */
  ProactorLogSink(Engine& engine, core::ConsumerCore& consumer, RotationType& rotation, SinkConfig config = {}) noexcept
      : engine_(&engine),
        handle_(engine, kSubsystemId),
        consumer_(&consumer),
        rotation_(&rotation),
        config_(config),
        idle_timeout_ts_(detail::ConfiguredTimespec(config.idle_timeout_ns)) {}

  /** @brief Construct a sink bound to an exclusive `LogConsumer` handle and opened `Rotation`. */
  ProactorLogSink(Engine& engine, core::LogConsumer& consumer, RotationType& rotation, SinkConfig config = {}) noexcept
      : engine_(&engine),
        handle_(engine, kSubsystemId),
        consumer_(ResolveConsumerCore(consumer)),
        rotation_(&rotation),
        config_(config),
        idle_timeout_ts_(detail::ConfiguredTimespec(config.idle_timeout_ns)) {}

  ProactorLogSink(const ProactorLogSink&) = delete ("ProactorLogSink holds pinned kernel SQE pointers");
  ProactorLogSink& operator=(const ProactorLogSink&) = delete ("ProactorLogSink holds pinned kernel SQE pointers");
  ProactorLogSink(ProactorLogSink&&) = delete ("ProactorLogSink holds pinned kernel SQE pointers");
  ProactorLogSink& operator=(ProactorLogSink&&) = delete ("ProactorLogSink holds pinned kernel SQE pointers");

  ~ProactorLogSink() noexcept {
    if (deep_park_active_) (void)core::ExitDeepPark();
    if (has_sinks_active_) (void)core::SetHasSinks(false);
    if (!Quiescent()) uring::sys::Panic("drain ProactorLogSink before destruction");
  }

  /** @brief Bind the rotation depth, arm the initial doorbell wait + idle timer, and service any queued logs. */
  [[nodiscard]] Result<void> Start() noexcept {
    if (uarch::unlikely(engine_ == nullptr || consumer_ == nullptr || rotation_ == nullptr || !rotation_->IsOpen())) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (uarch::unlikely(detail::LifecycleOf(flags_) != abi::LifecycleState::kIdle)) {
      return std::unexpected(ErrorCode::kConsumerBusy);
    }
    if (engine_->Config().mode.UsesIoPoll()) return std::unexpected(ErrorCode::kInvalidArgument);
    if (auto bound = io_fsm_.Bind(rotation_->MaxFiles()); !bound) return bound;
    if (config_.manage_deep_park != 0U) {
      [[maybe_unused]] const auto reg_mb = core::RegisterPrivateExpeditedMembarrier();
    }
    flags_ = detail::RunningReadyFlags();
    (void)core::SetHasSinks(true);
    has_sinks_active_ = true;
    need_drain_ = true;
    logical_now_ns_ = detail::MonotonicNowNs();
    return Service();
  }

  /**
   * @brief Consume a batch of completions routed by `uring` dispatcher and advance all three FSMs.
   */
  void OnCompletions(std::span<const uring::Delivery> deliveries) noexcept {
    for (const auto& delivery : deliveries) {
      if (const auto status = HandleOneDelivery(delivery); !status) {
        last_error_ = status.error();
      }
    }
    if (!IsStopped()) {
      if (const auto serviced = Service(); !serviced) {
        last_error_ = serviced.error();
      }
    }
  }

  /**
   * @brief Drain producer rings and stage due wake, timer, write, or rotation SQEs.
   */
  [[nodiscard]] Result<void> Service() noexcept {
    if (uarch::unlikely(consumer_ == nullptr || rotation_ == nullptr)) {
      return RecordError(ErrorCode::kInvalidArgument);
    }
    if (IsStopped()) {
      return std::unexpected(ErrorCode::kShuttingDown);
    }
    if (detail::LifecycleOf(flags_) != abi::LifecycleState::kRunning) {
      return RecordError(ErrorCode::kInvalidArgument);
    }

    if (detail::WriteRetryOf(flags_) && logical_now_ns_ >= write_retry_deadline_ns_ &&
        detail::IoOf(flags_) == abi::WriteRotateState::kReady) {
      auto action = io_fsm_.OnBackoffElapsed(flags_, *rotation_, true);
      if (!action) return RecordError(action.error());
      write_retry_deadline_ns_ = 0ULL;
      if (action->step != IoStep::kNone) {
        if (auto submitted = SubmitIoAction(*action); !submitted) return submitted;
      }
    }

    if (detail::RotationRetryOf(flags_) && logical_now_ns_ >= rotation_retry_deadline_ns_ &&
        detail::IoOf(flags_) == abi::WriteRotateState::kReady && io_fsm_.PendingLength() == 0U) {
      auto action = io_fsm_.OnBackoffElapsed(flags_, *rotation_, false);
      if (!action) return RecordError(action.error());
      rotation_retry_deadline_ns_ = 0ULL;
      if (action->step != IoStep::kNone) {
        if (auto submitted = SubmitIoAction(*action); !submitted) return submitted;
      }
    }

    if (detail::WakeOf(flags_) == abi::WakeState::kIdle) {
      if (auto armed = ArmWake(); !armed) return armed;
    }

    if (need_drain_) {
      if (auto drained = DrainRecordsCold(); !drained) return drained;
    }

    if (config_.manage_deep_park != 0U && !deep_park_active_ && empty_idle_ticks_ >= config_.manage_deep_park &&
        !more_pending_ && !backpressure_) {
      (void)core::EnterDeepPark();
      deep_park_active_ = true;
      ++deep_park_entries_;
      [[maybe_unused]] const auto mb = core::PrivateExpeditedMembarrier();
      if (auto rescanned = DrainRecordsCold(); !rescanned) return rescanned;
    }

    if (detail::IoOf(flags_) == abi::WriteRotateState::kReady) {
      if (io_fsm_.PendingLength() != 0U) {
        if (!detail::WriteRetryOf(flags_)) {
          auto action = io_fsm_.BeginWrite(flags_, io_fsm_.PendingLength());
          if (!action) return RecordError(action.error());
          if (action->step == IoStep::kWrite) {
            if (auto submitted = SubmitIoAction(*action); !submitted) return submitted;
          }
        }
      } else {
        const bool need_rotation = rotation_->NeedsRotation() || rotation_->OldestUnlinked() || rotation_->ReopenPending();
        if (need_rotation && !detail::RotationRetryOf(flags_)) {
          auto action = io_fsm_.BeginRotation(flags_, *rotation_);
          if (!action) return RecordError(action.error());
          if (action->step != IoStep::kNone && action->step != IoStep::kWaitBackoff) {
            if (auto submitted = SubmitIoAction(*action); !submitted) return submitted;
          }
        }
        if (detail::IoOf(flags_) == abi::WriteRotateState::kReady && !detail::WriteRetryOf(flags_) && rotation_->IsOpen() &&
            consumer_->Output().PendingBytes() != 0U && consumer_->Output().FrozenBytes() == 0U) {
          if (auto frozen = consumer_->Output().Freeze()) {
            in_flight_slab_ = *frozen;
            in_flight_length_ = frozen->length - frozen->offset;
            auto action = io_fsm_.BeginWrite(flags_, in_flight_length_);
            if (!action) return RecordError(action.error());
            if (action->step == IoStep::kWrite) {
              if (auto submitted = SubmitIoAction(*action); !submitted) return submitted;
            }
          }
        }
      }
    }

    if (more_pending_ && !backpressure_) {
      if (detail::TimerOf(flags_) == abi::TimerState::kIdle) {
        if (auto armed = ArmTimer(kImmediateTimeout, 0ULL); !armed) return armed;
      } else if (detail::TimerOf(flags_) == abi::TimerState::kArmed && !timer_fsm_.UpdateInFlight()) {
        if (auto updated = ArmImmediateTimerUpdate(); !updated) return updated;
      }
    } else if (detail::TimerOf(flags_) == abi::TimerState::kIdle) {
      const auto [ts, delay_ns] = SelectTimerSpec();
      if (auto armed = ArmTimer(ts, delay_ns); !armed) return armed;
    }

    return {};
  }

  /**
   * @brief Transition to `kStopping`, cancel `wake_key_` and `timer_key_`, and reconcile deferred cancellations.
   */
  [[nodiscard]] Result<void> RequestStop() noexcept {
    const auto life = detail::LifecycleOf(flags_);
    if (life == abi::LifecycleState::kIdle) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (deep_park_active_) {
      (void)core::ExitDeepPark();
      deep_park_active_ = false;
    }
    flags_ = detail::WithLifecycle(flags_, abi::LifecycleState::kStopping);

    Result<void> first_error{};
    auto record_cancel_err = [&](ErrorCode err) noexcept {
      if (first_error) first_error = std::unexpected(err);
      last_error_ = err;
    };

    if (timer_fsm_.UpdateInFlight() && timer_update_key_.token != uring::kNullToken) {
      const auto key = timer_update_key_;
      const auto canceled = handle_.Cancel(key);
      if (!canceled) {
        record_cancel_err(FromUring(canceled.error()));
      } else if (!engine_->Contains(key.token)) {
        timer_fsm_.ReconcileSyncUpdateCancel(flags_);
        timer_update_key_ = {};
      }
    }

    if (detail::TimerOf(flags_) == abi::TimerState::kUpdating) {
      flags_ = detail::WithTimer(flags_, abi::TimerState::kArmed);
    }
    if (TimerFsm::RequestCancel(flags_) && timer_key_.token != uring::kNullToken) {
      const auto key = timer_key_;
      const auto canceled = handle_.Cancel(key);
      if (!canceled) {
        record_cancel_err(FromUring(canceled.error()));
      } else if (!engine_->Contains(key.token)) {
        TimerFsm::ReconcileSyncCancel(flags_);
        timer_key_ = {};
      }
    }

    if (WakeFsm::RequestCancel(flags_) && wake_key_.token != uring::kNullToken) {
      const auto key = wake_key_;
      const auto canceled = handle_.Cancel(key);
      if (!canceled) {
        record_cancel_err(FromUring(canceled.error()));
      } else if (!engine_->Contains(key.token)) {
        WakeFsm::ReconcileSyncCancel(flags_);
        wake_key_ = {};
      }
    }

    if (io_key_.token != uring::kNullToken && !engine_->Contains(io_key_.token)) {
      io_key_ = {};
      in_flight_io_step_ = IoStep::kNone;
      WriteRotateFsm::YieldForShutdown(flags_);
    }

    return first_error;
  }

  /** @brief Overload accepting the bound `Engine&` explicitly. */
  [[nodiscard]] Result<void> RequestStop(Engine& engine) noexcept {
    if (uarch::unlikely(&engine != engine_)) return std::unexpected(ErrorCode::kInvalidArgument);
    return RequestStop();
  }

  /** @brief True when all asynchronous wake, timer, update, and I/O operations have retired. */
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[nodiscard]] bool Quiescent() const noexcept {
    const auto wake = detail::WakeOf(flags_);
    const auto timer = detail::TimerOf(flags_);
    const auto io = detail::IoOf(flags_);
    const bool io_quiescent = (io == abi::WriteRotateState::kIdle || io == abi::WriteRotateState::kReady);
    return wake == abi::WakeState::kIdle && timer == abi::TimerState::kIdle && !timer_fsm_.UpdateInFlight() && io_quiescent &&
           wake_key_.token == uring::kNullToken && timer_key_.token == uring::kNullToken &&
           timer_update_key_.token == uring::kNullToken && io_key_.token == uring::kNullToken;
  }

  /**
   * @brief Drain remaining records and flush/rotate synchronously after all async SQEs have retired (`Quiescent()`).
   */
  [[nodiscard]] Result<void> ShutdownFlushSync() noexcept {
    if (uarch::unlikely(consumer_ == nullptr || rotation_ == nullptr)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (uarch::unlikely(!Quiescent())) {
      return std::unexpected(ErrorCode::kConsumerBusy);
    }

    ErrorCode sticky_rotate_err = ErrorCode::kSuccess;
    auto drained = detail::DrainAndFlushSync(
        *consumer_, *rotation_, in_flight_slab_, config_.drain_budget, last_error_, sticky_rotate_err,
        [this]() noexcept { return FlushFrozenSlabSync(); }, [this]() noexcept { io_fsm_.CompleteSyncFlush(flags_); },
        [this]() noexcept { io_fsm_.CompleteSyncRotation(flags_); });
    if (!drained) return drained;

    if (detail::LifecycleOf(flags_) == abi::LifecycleState::kStopping) {
      flags_ = detail::WithLifecycle(flags_, abi::LifecycleState::kClosed);
    }
    if (has_sinks_active_) {
      (void)core::SetHasSinks(false);
      has_sinks_active_ = false;
    }
    if (sticky_rotate_err != ErrorCode::kSuccess) return std::unexpected(sticky_rotate_err);
    return {};
  }

  /** @brief Alias for `ShutdownFlushSync()` in Mode A shutdown sequences. */
  [[nodiscard]] Result<void> FlushPendingSync() noexcept { return ShutdownFlushSync(); }

  [[nodiscard]] abi::SinkFlagsWord Flags() const noexcept { return flags_; }
  [[nodiscard]] uring::OperationKey WakeKey() const noexcept { return wake_key_; }
  [[nodiscard]] uring::OperationKey TimerKey() const noexcept { return timer_key_; }
  [[nodiscard]] uring::OperationKey TimerUpdateKey() const noexcept { return timer_update_key_; }
  [[nodiscard]] uring::OperationKey IoKey() const noexcept { return io_key_; }
  [[nodiscard]] IoStep InFlightIoStep() const noexcept { return in_flight_io_step_; }
  [[nodiscard]] const WriteRotateFsm& IoFsm() const noexcept { return io_fsm_; }
  [[nodiscard]] const TimerFsm& TimerStateFsm() const noexcept { return timer_fsm_; }
  [[nodiscard]] ErrorCode LastError() const noexcept { return last_error_; }
  [[nodiscard]] ErrorCode TakeLastError() noexcept { return std::exchange(last_error_, ErrorCode::kSuccess); }
  [[nodiscard]] std::uint64_t DeepParkEntries() const noexcept { return deep_park_entries_; }
  [[nodiscard]] std::size_t TotalUnflushedBytes() const noexcept {
    return consumer_ != nullptr ? consumer_->Output().TotalUnflushedBytes() : 0U;
  }

 private:
  static constexpr uring::abi::KernelTimespec kImmediateTimeout{.tv_sec = 0, .tv_nsec = 0};

  [[nodiscard]] static core::ConsumerCore* ResolveConsumerCore(core::LogConsumer& consumer) noexcept {
    auto core = consumer.Core();
    return core ? *core : nullptr;
  }

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[nodiscard]] bool IsStopped() const noexcept {
    const auto life = detail::LifecycleOf(flags_);
    return life == abi::LifecycleState::kStopping || life == abi::LifecycleState::kDetached ||
           life == abi::LifecycleState::kClosed;
  }

  [[nodiscard]] Result<void> RecordError(ErrorCode err) noexcept {
    last_error_ = err;
    return std::unexpected(err);
  }

  [[nodiscard, gnu::noinline]] Result<void> DrainRecordsCold() noexcept {
    const bool from_timer = std::exchange(timer_expired_since_drain_, false);
    auto stats = consumer_->DrainBatch(sys::ReadTsc(), config_.drain_budget);
    if (!stats) return RecordError(stats.error());
    const auto drain_flags = abi::DrainFlagsWord::from_raw(stats->flags);
    more_pending_ = detail::DrainMorePending(drain_flags);
    backpressure_ = detail::DrainBackpressure(drain_flags);
    need_drain_ = false;
    if (stats->records != 0U) {
      empty_idle_ticks_ = 0U;
      if (deep_park_active_) {
        (void)core::ExitDeepPark();
        deep_park_active_ = false;
      }
    } else if (from_timer && !more_pending_ && !backpressure_) {
      ++empty_idle_ticks_;
    }
    return {};
  }

  [[nodiscard]] Result<void> HandleOneDelivery(const uring::Delivery& delivery) noexcept {
    const auto res = delivery.completion.result;
    if (delivery.key == wake_key_) {
      wake_key_ = {};
      empty_idle_ticks_ = 0U;
      if (deep_park_active_) {
        (void)core::ExitDeepPark();
        deep_park_active_ = false;
      }
      auto woke = WakeFsm::OnWakeCqe(flags_, res);
      if (!woke) return RecordError(woke.error());
      if (*woke) {
        (void)core::ConsumeDoorbellNotification();
        need_drain_ = true;
      }
      return {};
    }

    if (delivery.key == timer_key_) {
      timer_key_ = {};
      auto fired = TimerFsm::OnTimerCqe(flags_, res);
      if (!fired) return RecordError(fired.error());
      if (*fired) {
        logical_now_ns_ = std::max(logical_now_ns_, armed_timer_target_ns_);
        timer_expired_since_drain_ = true;
        need_drain_ = true;
      }
      return {};
    }

    if (delivery.key == timer_update_key_) {
      timer_update_key_ = {};
      auto updated = timer_fsm_.OnUpdateCqe(flags_, res);
      if (!updated) return RecordError(updated.error());
      return {};
    }

    if (delivery.key == io_key_) {
      io_key_ = {};
      const IoStep completed_step = std::exchange(in_flight_io_step_, IoStep::kNone);
      return HandleIoCompletion(completed_step, res);
    }

    return RecordError(ErrorCode::kInvalidArgument);
  }

  [[nodiscard]] Result<void> HandleIoCompletion(IoStep completed_step, std::int32_t res) noexcept {
    switch (completed_step) {
      case IoStep::kWrite: {
        if (res > 0) {
          const auto wrote = static_cast<std::uint32_t>(res);
          if (wrote <= in_flight_length_) {
            if (auto rec = rotation_->OnWriteDone(wrote); !rec) return RecordError(rec.error());
            auto completed = consumer_->Output().CompleteFrozen(in_flight_slab_, wrote);
            if (!completed) return RecordError(completed.error());
            if (*completed) {
              in_flight_slab_ = **completed;
            } else {
              in_flight_slab_ = {};
            }
            if (backpressure_) {
              need_drain_ = true;
            }
          }
        }
        auto next = io_fsm_.OnWriteCqe(flags_, res, in_flight_length_);
        if (!next) {
          write_retry_deadline_ns_ = logical_now_ns_ + static_cast<std::uint64_t>(io_fsm_.WriteBackoffMs()) * 1'000'000ULL;
          return RecordError(next.error());
        }
        in_flight_length_ = next->length;
        if (next->step == IoStep::kWrite) {
          if (IsStopped()) {
            WriteRotateFsm::YieldForShutdown(flags_);
            return {};
          }
          return SubmitIoAction(*next);
        }
        return {};
      }

      case IoStep::kUnlinkOldest:
      case IoStep::kRename: {
        auto next = (completed_step == IoStep::kUnlinkOldest) ? io_fsm_.OnUnlinkCqe(flags_, *rotation_, res)
                                                              : io_fsm_.OnRenameCqe(flags_, *rotation_, res);
        if (!next) {
          rotation_retry_deadline_ns_ =
              logical_now_ns_ + static_cast<std::uint64_t>(io_fsm_.RotationBackoffMs()) * 1'000'000ULL;
          return RecordError(next.error());
        }
        if (IsStopped()) {
          WriteRotateFsm::YieldForShutdown(flags_);
          return {};
        }
        return SubmitIoAction(*next);
      }

      case IoStep::kOpenNew: {
        auto next = io_fsm_.OnOpenCqe(flags_, *rotation_, res);
        if (!next) {
          rotation_retry_deadline_ns_ =
              logical_now_ns_ + static_cast<std::uint64_t>(io_fsm_.RotationBackoffMs()) * 1'000'000ULL;
          return RecordError(next.error());
        }
        if (next->step == IoStep::kCloseOld) {
          if (IsStopped()) {
            auto retired = uring::sys::UniqueFd::Adopt(next->close_fd);
            (void)retired;
            auto closed = io_fsm_.OnCloseCqe(flags_, 0);
            if (!closed) return RecordError(closed.error());
            return {};
          }
          return SubmitIoAction(*next);
        }
        return {};
      }

      case IoStep::kCloseOld: {
        auto next = io_fsm_.OnCloseCqe(flags_, res);
        if (!next) return RecordError(next.error());
        return {};
      }

      case IoStep::kNone:
      case IoStep::kWaitBackoff:
        return RecordError(ErrorCode::kInvalidArgument);
    }
    return RecordError(ErrorCode::kInvalidArgument);
  }

  template <class Op>
  [[nodiscard]] Result<void> ArmRotationStep(const Op& op, IoStep step) noexcept {
    auto armed = handle_.Arm(op);
    if (!armed) {
      io_fsm_.RollbackUnsubmitted(flags_, false);
      rotation_retry_deadline_ns_ = logical_now_ns_ + static_cast<std::uint64_t>(io_fsm_.RotationBackoffMs()) * 1'000'000ULL;
      return RecordError(FromUring(armed.error()));
    }
    io_key_ = uring::OperationKey{*armed, engine_->Instance()};
    in_flight_io_step_ = step;
    return {};
  }

  [[nodiscard]] Result<void> SubmitIoAction(const IoAction& action) noexcept {
    switch (action.step) {
      case IoStep::kWrite: {
        const auto op = rotation_->PrepareWrite(in_flight_slab_.data + in_flight_slab_.offset, action.length);
        auto armed = handle_.Arm(op);
        if (!armed) {
          io_fsm_.RollbackUnsubmitted(flags_, true);
          write_retry_deadline_ns_ = logical_now_ns_ + static_cast<std::uint64_t>(io_fsm_.WriteBackoffMs()) * 1'000'000ULL;
          return RecordError(FromUring(armed.error()));
        }
        io_key_ = uring::OperationKey{*armed, engine_->Instance()};
        in_flight_io_step_ = IoStep::kWrite;
        return {};
      }
      case IoStep::kUnlinkOldest:
        return ArmRotationStep(rotation_->PrepareUnlinkOldest(), IoStep::kUnlinkOldest);
      case IoStep::kRename:
        return ArmRotationStep(rotation_->PrepareRenameStep(action.rename_slot), IoStep::kRename);
      case IoStep::kOpenNew:
        return ArmRotationStep(rotation_->PrepareOpenNew(), IoStep::kOpenNew);
      case IoStep::kCloseOld: {
        const auto op = uring::ops::PrepareClose(action.close_fd);
        auto armed = handle_.Arm(op);
        if (!armed) {
          [[maybe_unused]] const auto retired = uring::sys::UniqueFd::Adopt(action.close_fd);
          [[maybe_unused]] const auto closed = io_fsm_.OnCloseCqe(flags_, 0);
          return RecordError(FromUring(armed.error()));
        }
        io_key_ = uring::OperationKey{*armed, engine_->Instance()};
        in_flight_io_step_ = IoStep::kCloseOld;
        return {};
      }
      case IoStep::kNone:
      case IoStep::kWaitBackoff:
        return {};
    }
    return {};
  }

  [[nodiscard]] Result<void> ArmWake() noexcept {
    if (auto tried = WakeFsm::TryArm(flags_); !tried) return RecordError(tried.error());
    observed_ = core::LoadDoorbell(std::memory_order_acquire);
    while (!core::TryParkDoorbell(observed_)) {
    }
    const auto op = uring::ops::PrepareFutexWait<abi::kDoorbellFutexFlags>(&core::g_doorbell->value, observed_.raw());
    auto armed = handle_.Arm(op);
    if (!armed) {
      (void)WakeFsm::RequestCancel(flags_);
      WakeFsm::ReconcileSyncCancel(flags_);
      return RecordError(FromUring(armed.error()));
    }
    wake_key_ = uring::OperationKey{*armed, engine_->Instance()};
    return {};
  }

  [[nodiscard]] std::pair<uring::abi::KernelTimespec, std::uint64_t> SelectTimerSpec() const noexcept {
    std::uint32_t min_backoff_ms = 0U;
    if (detail::WriteRetryOf(flags_) && io_fsm_.WriteBackoffMs() != 0U) {
      min_backoff_ms = io_fsm_.WriteBackoffMs();
    }
    if (detail::RotationRetryOf(flags_) && io_fsm_.RotationBackoffMs() != 0U) {
      if (min_backoff_ms == 0U || io_fsm_.RotationBackoffMs() < min_backoff_ms) {
        min_backoff_ms = io_fsm_.RotationBackoffMs();
      }
    }
    if (min_backoff_ms != 0U) {
      const auto delay_ns = static_cast<std::uint64_t>(min_backoff_ms) * 1'000'000ULL;
      if (delay_ns <= config_.idle_timeout_ns) {
        return {detail::BackoffTimespec(min_backoff_ms), delay_ns};
      }
    }
    return {idle_timeout_ts_, config_.idle_timeout_ns};
  }

  [[nodiscard]] Result<void> ArmTimer(uring::abi::KernelTimespec spec, std::uint64_t delay_ns) noexcept {
    if (auto tried = TimerFsm::TryArm(flags_); !tried) return RecordError(tried.error());
    timer_ts_ = spec;
    armed_timer_target_ns_ = logical_now_ns_ + delay_ns;
    const auto op = uring::ops::PrepareTimeout(uring::ops::UserAddress{&timer_ts_}, 0U);
    auto armed = handle_.Arm(op);
    if (!armed) {
      (void)TimerFsm::RequestCancel(flags_);
      TimerFsm::ReconcileSyncCancel(flags_);
      return RecordError(FromUring(armed.error()));
    }
    timer_key_ = uring::OperationKey{*armed, engine_->Instance()};
    return {};
  }

  [[nodiscard]] Result<void> ArmImmediateTimerUpdate() noexcept {
    if (auto tried = timer_fsm_.RequestImmediateUpdate(flags_); !tried) return RecordError(tried.error());
    armed_timer_target_ns_ = logical_now_ns_;
    const auto op = uring::ops::PrepareTimeoutUpdate(timer_key_.token, uring::ops::UserAddress{&kImmediateTimeout});
    auto armed = handle_.Arm(op);
    if (!armed) {
      timer_fsm_.ReconcileSyncUpdateCancel(flags_);
      return RecordError(FromUring(armed.error()));
    }
    timer_update_key_ = uring::OperationKey{*armed, engine_->Instance()};
    return {};
  }

  [[nodiscard]] Result<void> FlushFrozenSlabSync() noexcept {
    auto result = detail::FlushFrozenSlabSync(*consumer_, *rotation_, in_flight_slab_);
    in_flight_length_ = in_flight_slab_.length - in_flight_slab_.offset;
    return result;
  }

  Engine* engine_{};
  uring::SubsystemHandle<Engine> handle_;
  core::ConsumerCore* consumer_{};
  RotationType* rotation_{};
  SinkConfig config_{};
  uring::abi::KernelTimespec idle_timeout_ts_{};
  uring::abi::KernelTimespec timer_ts_{};
  uring::OperationKey wake_key_{};
  uring::OperationKey timer_key_{};
  uring::OperationKey timer_update_key_{};
  uring::OperationKey io_key_{};
  format::FrozenSlab in_flight_slab_{};
  std::uint32_t in_flight_length_{};
  IoStep in_flight_io_step_{IoStep::kNone};
  abi::SinkFlagsWord flags_{};
  abi::DoorbellWord observed_{};
  TimerFsm timer_fsm_{};
  WriteRotateFsm io_fsm_{};
  std::uint64_t logical_now_ns_{};
  std::uint64_t armed_timer_target_ns_{};
  std::uint64_t write_retry_deadline_ns_{};
  std::uint64_t rotation_retry_deadline_ns_{};
  std::uint64_t deep_park_entries_{0U};
  std::uint32_t empty_idle_ticks_{0U};
  ErrorCode last_error_{ErrorCode::kSuccess};
  bool need_drain_{false};
  bool more_pending_{false};
  bool backpressure_{false};
  bool deep_park_active_{false};
  bool has_sinks_active_{false};
  bool timer_expired_since_drain_{false};
};

static_assert(uring::plugins::Subsystem<ProactorLogSink<uring::Proactor, 1>>);
static_assert(uring::plugins::Subsystem<ProactorLogSink<uring::ScriptedProactor, 1>>);

}  // namespace zerolog::sink

namespace zerolog {
using sink::ProactorLogSink;
using sink::SinkConfig;
using sink::SubsystemId;
}  // namespace zerolog
