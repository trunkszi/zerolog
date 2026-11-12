/** @file @brief Synchronous `futex2::Wait` consumer loop with independent write and rotation backoff windows. */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <uarch/uarch.hpp>
#include <uring/abi/time.hpp>
#include <uring/futex2/futex2.hpp>
#include <utility>
#include <zerolog/abi/contract.hpp>
#include <zerolog/core/consumer.hpp>
#include <zerolog/core/consumer_core.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/format/output_buffer.hpp>
#include <zerolog/sink/proactor_sink.hpp>
#include <zerolog/sink/rotation.hpp>
#include <zerolog/sys/tsc_clock.hpp>

namespace zerolog::sink {

/**
 * @brief Synchronous fallback sink using `uring::futex2::Wait` on `core::g_doorbell` plus `Rotation::WriteSync`
 * and `Rotation::RotateSync` with independent `1 ms .. 1000 ms` exponential backoff windows.
 */
template <class RotationType = Rotation>
class FutexFallbackSink final {
 public:
  /** @brief Construct a fallback sink bound to a `ConsumerCore` and opened `Rotation`. */
  FutexFallbackSink(core::ConsumerCore& consumer, RotationType& rotation, SinkConfig config = {}) noexcept
      : consumer_(&consumer), rotation_(&rotation), config_(config) {}

  /** @brief Construct a fallback sink bound to an exclusive `LogConsumer` and opened `Rotation`. */
  FutexFallbackSink(core::LogConsumer& consumer, RotationType& rotation, SinkConfig config = {}) noexcept
      : consumer_(ResolveConsumerCore(consumer)), rotation_(&rotation), config_(config) {}

  FutexFallbackSink(const FutexFallbackSink&) = delete ("FutexFallbackSink holds pinned FrozenSlab state");
  FutexFallbackSink& operator=(const FutexFallbackSink&) = delete ("FutexFallbackSink holds pinned FrozenSlab state");
  FutexFallbackSink(FutexFallbackSink&&) = delete ("FutexFallbackSink holds pinned FrozenSlab state");
  FutexFallbackSink& operator=(FutexFallbackSink&&) = delete ("FutexFallbackSink holds pinned FrozenSlab state");

  ~FutexFallbackSink() noexcept {
    if (deep_park_active_) {
      (void)core::ExitDeepPark();
      deep_park_active_ = false;
    }
    if (has_sinks_active_) {
      (void)core::SetHasSinks(false);
      has_sinks_active_ = false;
    }
  }

  /**
   * @brief Execute one non-blocking drain, write, and rotation step at timestamp @p now_ns.
   * @return `true` when `DrainBatch` reported `more_pending` without slab backpressure.
   */
  [[nodiscard]] Result<bool> Step(std::uint64_t now_ns) noexcept {
    if (uarch::unlikely(consumer_ == nullptr || rotation_ == nullptr)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (!has_sinks_active_) {
      (void)core::SetHasSinks(true);
      has_sinks_active_ = true;
    }

    if (write_retry_ && now_ns >= write_retry_deadline_ns_) {
      write_retry_ = false;
      write_retry_deadline_ns_ = 0ULL;
    }
    if (rotation_retry_ && now_ns >= rotation_retry_deadline_ns_) {
      rotation_retry_ = false;
      rotation_retry_deadline_ns_ = 0ULL;
    }

    (void)core::ConsumeDoorbellNotification();
    auto stats = consumer_->DrainBatch(sys::ReadTsc(), config_.drain_budget);
    if (!stats) {
      last_error_ = stats.error();
      return std::unexpected(stats.error());
    }
    const auto drain_flags = abi::DrainFlagsWord::from_raw(stats->flags);
    const bool more_pending = detail::DrainMorePending(drain_flags);
    const bool backpressure = detail::DrainBackpressure(drain_flags);
    if (stats->records != 0U) {
      empty_idle_ticks_ = 0U;
      if (deep_park_active_) {
        (void)core::ExitDeepPark();
        deep_park_active_ = false;
      }
    } else if (!more_pending && !backpressure) {
      ++empty_idle_ticks_;
    }

    ServiceOutput(now_ns);
    return more_pending && !backpressure && !write_retry_;
  }

  /**
   * @brief Park on `core::g_doorbell` via `uring::futex2::Wait` until woken, stopped, or deadline expires.
   */
  [[nodiscard]] Result<uring::futex2::WaitOutcome> WaitOnce(const std::atomic<bool>& stop_requested,
                                                            std::uint64_t now_ns) noexcept {
    if (stop_requested.load(std::memory_order_acquire)) {
      return uring::futex2::WaitOutcome::kWoken;
    }
    bool entered_deep_park = false;
    if (config_.manage_deep_park != 0U && !deep_park_active_ && empty_idle_ticks_ >= config_.manage_deep_park) {
      if (!membarrier_registered_) {
        [[maybe_unused]] const auto reg_mb = core::RegisterPrivateExpeditedMembarrier();
        membarrier_registered_ = true;
      }
      (void)core::EnterDeepPark();
      deep_park_active_ = true;
      entered_deep_park = true;
      ++deep_park_entries_;
    }
    abi::DoorbellWord observed = core::LoadDoorbell(std::memory_order_acquire);
    while (!core::TryParkDoorbell(observed)) {
    }
    if (entered_deep_park) {
      [[maybe_unused]] const auto mb = core::PrivateExpeditedMembarrier();
      auto stats = consumer_->DrainBatch(sys::ReadTsc(), config_.drain_budget);
      if (!stats) {
        if (deep_park_active_) {
          (void)core::ExitDeepPark();
          deep_park_active_ = false;
        }
        last_error_ = stats.error();
        return std::unexpected(stats.error());
      }
      ServiceOutput(now_ns);
      const auto drain_flags = abi::DrainFlagsWord::from_raw(stats->flags);
      if (stats->records != 0U || detail::DrainMorePending(drain_flags)) {
        empty_idle_ticks_ = 0U;
        if (deep_park_active_) {
          (void)core::ExitDeepPark();
          deep_park_active_ = false;
        }
        return uring::futex2::WaitOutcome::kWoken;
      }
    }
    if (stop_requested.load(std::memory_order_acquire)) {
      if (deep_park_active_) {
        (void)core::ExitDeepPark();
        deep_park_active_ = false;
      }
      return uring::futex2::WaitOutcome::kWoken;
    }

    auto ref = core::DoorbellFutexRef();
    if (!ref) {
      if (deep_park_active_) {
        (void)core::ExitDeepPark();
        deep_park_active_ = false;
      }
      const auto err = FromUring(ref.error());
      last_error_ = err;
      return std::unexpected(err);
    }

    const std::uint64_t wait_ns = NextWaitDelayNs(now_ns);
    const auto deadline = MakeMonotonicDeadline(now_ns + wait_ns);
    auto waited = uring::futex2::Wait(*ref, observed.raw(), deadline);
    if (!waited) {
      if (deep_park_active_) {
        (void)core::ExitDeepPark();
        deep_park_active_ = false;
      }
      const auto err = FromUring(waited.error());
      last_error_ = err;
      return std::unexpected(err);
    }
    if (*waited == uring::futex2::WaitOutcome::kWoken) {
      empty_idle_ticks_ = 0U;
      if (deep_park_active_) {
        (void)core::ExitDeepPark();
        deep_park_active_ = false;
      }
    }
    return *waited;
  }

  /**
   * @brief Run the synchronous consumer loop until @p stop_requested becomes true, then flush remaining records.
   */
  [[nodiscard]] Result<void> Run(const std::atomic<bool>& stop_requested) noexcept {
    if (uarch::unlikely(consumer_ == nullptr || rotation_ == nullptr || !rotation_->IsOpen())) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    (void)core::SetHasSinks(true);
    has_sinks_active_ = true;

    while (!stop_requested.load(std::memory_order_acquire)) {
      const std::uint64_t now_ns = detail::MonotonicNowNs();
      auto stepped = Step(now_ns);
      if (!stepped) {
        return std::unexpected(stepped.error());
      }
      if (*stepped || stop_requested.load(std::memory_order_acquire)) {
        continue;
      }
      const std::uint64_t wait_now_ns = detail::MonotonicNowNs();
      auto waited = WaitOnce(stop_requested, wait_now_ns);
      if (!waited) {
        return std::unexpected(waited.error());
      }
    }

    return ShutdownFlushSync();
  }

  /** @brief Drain all remaining records and flush/rotate synchronously on shutdown. */
  [[nodiscard]] Result<void> ShutdownFlushSync() noexcept {
    if (uarch::unlikely(consumer_ == nullptr || rotation_ == nullptr)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (deep_park_active_) {
      (void)core::ExitDeepPark();
      deep_park_active_ = false;
    }

    ErrorCode sticky_rotate_err = ErrorCode::kSuccess;
    auto drained = detail::DrainAndFlushSync(
        *consumer_, *rotation_, in_flight_slab_, config_.drain_budget, last_error_, sticky_rotate_err,
        [this]() noexcept { return FlushFrozenSlabSync(); }, [this]() noexcept { ClearWriteBackoff(); },
        [this]() noexcept { ClearRotationBackoff(); });
    if (!drained) return drained;

    if (has_sinks_active_) {
      (void)core::SetHasSinks(false);
      has_sinks_active_ = false;
    }
    if (sticky_rotate_err != ErrorCode::kSuccess) return std::unexpected(sticky_rotate_err);
    return {};
  }

  [[nodiscard]] bool WriteRetryActive() const noexcept { return write_retry_; }
  [[nodiscard]] bool RotationRetryActive() const noexcept { return rotation_retry_; }
  [[nodiscard]] std::uint32_t WriteBackoffMs() const noexcept { return write_backoff_ms_; }
  [[nodiscard]] std::uint32_t RotationBackoffMs() const noexcept { return rotation_backoff_ms_; }
  [[nodiscard]] ErrorCode LastError() const noexcept { return last_error_; }
  [[nodiscard]] ErrorCode TakeLastError() noexcept { return std::exchange(last_error_, ErrorCode::kSuccess); }
  [[nodiscard]] std::uint64_t DeepParkEntries() const noexcept { return deep_park_entries_; }
  [[nodiscard]] std::size_t TotalUnflushedBytes() const noexcept {
    return consumer_ != nullptr ? consumer_->Output().TotalUnflushedBytes() : 0U;
  }

 private:
  [[nodiscard]] static core::ConsumerCore* ResolveConsumerCore(core::LogConsumer& consumer) noexcept {
    auto core = consumer.Core();
    return core ? *core : nullptr;
  }

  void ArmWriteBackoff(std::uint64_t now_ns, ErrorCode err) noexcept {
    last_error_ = err;
    write_backoff_ms_ = detail::BumpBackoffMs(write_shift_);
    write_retry_ = true;
    write_retry_deadline_ns_ = now_ns + static_cast<std::uint64_t>(write_backoff_ms_) * 1'000'000ULL;
  }

  void ClearWriteBackoff() noexcept {
    write_shift_ = 0U;
    write_backoff_ms_ = 0U;
    write_retry_ = false;
    write_retry_deadline_ns_ = 0ULL;
  }

  void ArmRotationBackoff(std::uint64_t now_ns, ErrorCode err) noexcept {
    last_error_ = err;
    rotation_backoff_ms_ = detail::BumpBackoffMs(rotation_shift_);
    rotation_retry_ = true;
    rotation_retry_deadline_ns_ = now_ns + static_cast<std::uint64_t>(rotation_backoff_ms_) * 1'000'000ULL;
  }

  void ClearRotationBackoff() noexcept {
    rotation_shift_ = 0U;
    rotation_backoff_ms_ = 0U;
    rotation_retry_ = false;
    rotation_retry_deadline_ns_ = 0ULL;
  }

  void ServiceOutput(std::uint64_t now_ns) noexcept {
    for (;;) {
      if (in_flight_slab_.length == 0U) {
        const bool need_rotation = rotation_->NeedsRotation() || rotation_->OldestUnlinked() || rotation_->ReopenPending();
        if (need_rotation && !rotation_retry_) {
          if (auto rotated = rotation_->RotateSync(); !rotated) {
            ArmRotationBackoff(now_ns, rotated.error());
            if (!rotation_->IsOpen()) return;
          } else {
            ClearRotationBackoff();
          }
        }
        if (write_retry_ || !rotation_->IsOpen() || consumer_->Output().PendingBytes() == 0U) {
          return;
        }
        auto frozen = consumer_->Output().Freeze();
        if (!frozen) return;
        in_flight_slab_ = *frozen;
      }

      if (write_retry_ || !rotation_->IsOpen()) {
        return;
      }

      const std::span<const char> span{in_flight_slab_.data + in_flight_slab_.offset,
                                       in_flight_slab_.length - in_flight_slab_.offset};
      const auto progress = rotation_->WriteSync(span);
      if (progress.written > 0U) {
        auto completed = consumer_->Output().CompleteFrozen(in_flight_slab_, progress.written);
        if (!completed) {
          last_error_ = completed.error();
          return;
        }
        if (*completed) {
          in_flight_slab_ = **completed;
        } else {
          in_flight_slab_ = {};
        }
      }

      if (progress.error != ErrorCode::kSuccess) {
        ArmWriteBackoff(now_ns, progress.error);
        return;
      }

      ClearWriteBackoff();
      if (rotation_->NeedsRotation() && !rotation_retry_) {
        if (auto rotated = rotation_->RotateSync(); !rotated) {
          ArmRotationBackoff(now_ns, rotated.error());
        } else {
          ClearRotationBackoff();
        }
      }
      if (consumer_->Output().PendingBytes() == 0U) {
        return;
      }
    }
  }

  [[nodiscard]] Result<void> FlushFrozenSlabSync() noexcept {
    auto result = detail::FlushFrozenSlabSync(*consumer_, *rotation_, in_flight_slab_);
    return result;
  }

  [[nodiscard]] std::uint64_t NextWaitDelayNs(std::uint64_t now_ns) const noexcept {
    std::uint64_t wait_ns = config_.idle_timeout_ns;
    if (write_retry_) {
      const std::uint64_t rem = write_retry_deadline_ns_ > now_ns ? (write_retry_deadline_ns_ - now_ns) : 0ULL;
      wait_ns = std::min(wait_ns, rem);
    }
    if (rotation_retry_) {
      const std::uint64_t rem = rotation_retry_deadline_ns_ > now_ns ? (rotation_retry_deadline_ns_ - now_ns) : 0ULL;
      wait_ns = std::min(wait_ns, rem);
    }
    return wait_ns;
  }

  [[nodiscard]] static uring::abi::AbsoluteDeadline MakeMonotonicDeadline(std::uint64_t target_ns) noexcept {
    const auto ts = detail::ConfiguredTimespec(target_ns);
    return uring::abi::AbsoluteDeadline{
        .time = ts,
        .clock = uring::abi::FutexClock::kMonotonic,
        .reserved = 0U,
    };
  }

  core::ConsumerCore* consumer_{};
  RotationType* rotation_{};
  SinkConfig config_{};
  format::FrozenSlab in_flight_slab_{};
  std::uint64_t write_retry_deadline_ns_{};
  std::uint64_t rotation_retry_deadline_ns_{};
  std::uint64_t deep_park_entries_{0U};
  std::uint32_t empty_idle_ticks_{0U};
  std::uint32_t write_backoff_ms_{};
  std::uint32_t rotation_backoff_ms_{};
  std::uint8_t write_shift_{};
  std::uint8_t rotation_shift_{};
  ErrorCode last_error_{ErrorCode::kSuccess};
  bool write_retry_{false};
  bool rotation_retry_{false};
  bool deep_park_active_{false};
  bool has_sinks_active_{false};
  bool membarrier_registered_{false};
};

/** @brief Run the synchronous futex2 fallback consumer loop over a `ConsumerCore`. */
template <class RotationType = Rotation>
[[nodiscard]] inline Result<void> RunFutexFallback(core::ConsumerCore& consumer, RotationType& rotation,
                                                   const std::atomic<bool>& stop_requested, SinkConfig config = {}) noexcept {
  FutexFallbackSink<RotationType> fallback{consumer, rotation, config};
  return fallback.Run(stop_requested);
}

/** @brief Run the synchronous futex2 fallback consumer loop over an exclusive `LogConsumer`. */
template <class RotationType = Rotation>
[[nodiscard]] inline Result<void> RunFutexFallback(core::LogConsumer& consumer, RotationType& rotation,
                                                   const std::atomic<bool>& stop_requested, SinkConfig config = {}) noexcept {
  FutexFallbackSink<RotationType> fallback{consumer, rotation, config};
  return fallback.Run(stop_requested);
}

}  // namespace zerolog::sink

namespace zerolog {
using sink::FutexFallbackSink;
using sink::RunFutexFallback;
}  // namespace zerolog
