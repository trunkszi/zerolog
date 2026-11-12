/** @file @brief Single in-flight write and rotation steps, with separate backoff windows. */
#pragma once

#include <cerrno>
#include <cstdint>
#include <uarch/uarch.hpp>
#include <zerolog/abi/contract.hpp>
#include <zerolog/core/consumer_core.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/sink/rotation.hpp>
#include <zerolog/sink/wake_timer_fsm.hpp>
#include <zerolog/sys/tsc_clock.hpp>

namespace zerolog::sink {

namespace detail {
/** @brief Shared bounded exponential retry schedule. */
[[nodiscard]] inline std::uint32_t BumpBackoffMs(std::uint8_t& shift) noexcept {
  if (shift >= 10U) return 1000U;
  const auto delay = std::uint32_t{1} << shift;
  ++shift;
  return delay > 1000U ? 1000U : delay;
}

/** @brief Account for each written prefix before propagating a synchronous write error. */
template <class RotationType>
[[nodiscard]] Result<void> FlushFrozenSlabSync(core::ConsumerCore& consumer, RotationType& rotation,
                                               format::FrozenSlab& slab) noexcept {
  while (slab.length > slab.offset) {
    const std::span<const char> bytes{slab.data + slab.offset, slab.length - slab.offset};
    const auto progress = rotation.WriteSync(bytes);
    if (progress.written > 0U) {
      auto completed = consumer.Output().CompleteFrozen(slab, progress.written);
      if (!completed) return std::unexpected(completed.error());
      slab = *completed ? **completed : format::FrozenSlab{};
    }
    if (progress.error != ErrorCode::kSuccess) return std::unexpected(progress.error);
  }
  return {};
}

/** @brief A failed reopen retains the old descriptor so shutdown can preserve all buffered output. */
template <class RotationType, class Flush, class WriteDone, class RotationDone>
[[nodiscard]] Result<void> DrainAndFlushSync(core::ConsumerCore& consumer, RotationType& rotation, format::FrozenSlab& slab,
                                             abi::DrainBudget budget, ErrorCode& last_error, ErrorCode& sticky_rotate_error,
                                             Flush flush, WriteDone write_done, RotationDone rotation_done) noexcept {
  if (slab.length > slab.offset) {
    if (auto flushed = flush(); !flushed) return flushed;
    write_done();
  }
  auto try_rotate = [&]() noexcept -> Result<void> {
    if (sticky_rotate_error != ErrorCode::kSuccess) return {};
    if (auto rotated = rotation.RotateSync(); !rotated) {
      sticky_rotate_error = rotated.error();
      last_error = rotated.error();
      if (!rotation.IsOpen()) return std::unexpected(rotated.error());
      return {};
    }
    rotation_done();
    return {};
  };
  if (rotation.OldestUnlinked() || rotation.ReopenPending() || rotation.NeedsRotation()) {
    if (auto rotated = try_rotate(); !rotated) return rotated;
  }
  for (;;) {
    auto stats = consumer.DrainBatch(sys::ReadTsc(), budget);
    if (!stats) return std::unexpected(stats.error());
    while (consumer.Output().PendingBytes() != 0U) {
      if (rotation.NeedsRotation() || rotation.OldestUnlinked() || rotation.ReopenPending()) {
        if (auto rotated = try_rotate(); !rotated) return rotated;
      }
      auto frozen = consumer.Output().Freeze();
      if (!frozen) break;
      slab = *frozen;
      if (auto flushed = flush(); !flushed) return flushed;
      write_done();
      if (rotation.NeedsRotation()) {
        if (auto rotated = try_rotate(); !rotated) return rotated;
      }
    }
    const auto flags = abi::DrainFlagsWord::from_raw(stats->flags);
    if (!detail::DrainHasMore(flags) && stats->records == 0U) break;
  }
  return {};
}
}  // namespace detail

/** @brief Next submission the caller should issue. `kWaitBackoff` submits nothing. */
enum class IoStep : std::uint8_t {
  kNone = 0,
  kWrite,
  kUnlinkOldest,
  kRename,
  kOpenNew,
  kCloseOld,
  kWaitBackoff,
};

/** @brief One transition result. `close_fd` is transferred to the caller when `step == kCloseOld`. */
struct IoAction {
  IoStep step{IoStep::kNone};  ///< Submission the caller issues next.
  std::uint8_t rename_slot{};  ///< Source index for `kRename`.
  std::uint16_t reserved{};    ///< Keeps the action a 16-byte register pair.
  std::uint32_t length{};      ///< Byte count for `kWrite`.
  std::uint32_t backoff_ms{};  ///< Delay for `kWaitBackoff`.
  std::int32_t close_fd{-1};   ///< Previous descriptor for `kCloseOld`, or `-1`.
};
static_assert(sizeof(IoAction) == 16 && uarch::NoPadding<IoAction> && uarch::GprPair<IoAction>);

/**
 * @brief Write and rotation progress stored in `abi::SinkFlagsWord` plus the rotation cursor.
 * Short writes stay on the suffix and do not rotate. An `openat` failure sets `rotation_retry` and
 * returns to `kReady`, so appends to the live descriptor continue until that window elapses.
 * Backoff is `1 ms` doubled until `1000 ms`, by a shift, with no division.
 */
class WriteRotateFsm {
 public:
  /** @brief Archive depth used to reject a rename slot outside `0 .. max_files - 1`. */
  [[nodiscard]] Result<void> Bind(std::uint8_t max_files) noexcept {
    if (max_files == 0 || max_files > 15) return std::unexpected(ErrorCode::kInvalidArgument);
    max_files_ = max_files;
    bound_ = 1;
    return {};
  }

  /**
   * @brief Begin a write of `length` bytes, or report the active write backoff.
   * A sink step that is already in flight returns `kConsumerBusy`.
   */
  [[nodiscard]] Result<IoAction> BeginWrite(zerolog::abi::SinkFlagsWord& flags, std::uint32_t length) noexcept {
    if (length == 0 || Stopped(flags))
      return std::unexpected(length == 0 ? ErrorCode::kInvalidArgument : ErrorCode::kShuttingDown);
    if (Busy(flags)) return std::unexpected(ErrorCode::kConsumerBusy);
    if (flags.test<^^zerolog::abi::SinkFlagsSpec::write_retry>()) {
      return IoAction{.step = IoStep::kWaitBackoff, .backoff_ms = write_backoff_ms_};
    }
    if (pending_length_ != 0 && length != pending_length_) return std::unexpected(ErrorCode::kInvalidArgument);
    pending_length_ = length;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::io>(zerolog::abi::WriteRotateState::kWriting);
    return IoAction{.step = IoStep::kWrite, .length = length};
  }

  /**
   * @brief Consume one write completion of an in-flight span of `length` bytes.
   * `0 < res < length` rearms the suffix and does not rotate. A negative result arms the write window.
   */
  [[nodiscard]] Result<IoAction> OnWriteCqe(zerolog::abi::SinkFlagsWord& flags, std::int32_t res,
                                            std::uint32_t length) noexcept {
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::io>() != zerolog::abi::WriteRotateState::kWriting) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (length == 0 || length != pending_length_) return std::unexpected(ErrorCode::kInvalidArgument);
    if (res < 0 || res == 0) {
      ArmWrite(flags);
      return std::unexpected(res == 0 ? ErrorCode::kShortWrite : ErrorCode::kWriteFailed);
    }
    const auto wrote = static_cast<std::uint32_t>(res);
    if (wrote > length) {
      pending_length_ = 0;
      flags = flags.with<^^zerolog::abi::SinkFlagsSpec::io>(zerolog::abi::WriteRotateState::kReady);
      return std::unexpected(ErrorCode::kWriteFailed);
    }
    if (wrote < length) {
      pending_length_ = length - wrote;
      return IoAction{.step = IoStep::kWrite, .length = pending_length_};
    }
    pending_length_ = 0;
    write_shift_ = 0;
    write_backoff_ms_ = 0;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::write_retry>(false).with<^^zerolog::abi::SinkFlagsSpec::io>(
        zerolog::abi::WriteRotateState::kReady);
    return IoAction{};
  }

  /** @brief Start or resume rotation from the cursor already stored on `rotation`. */
  template <class Rotation>
  [[nodiscard]] Result<IoAction> BeginRotation(zerolog::abi::SinkFlagsWord& flags, Rotation& rotation) noexcept {
    if (bound_ == 0 || Stopped(flags))
      return std::unexpected(bound_ == 0 ? ErrorCode::kInvalidArgument : ErrorCode::kShuttingDown);
    if (Busy(flags)) return std::unexpected(ErrorCode::kConsumerBusy);
    if (RotationRetry(flags)) {
      return IoAction{.step = IoStep::kWaitBackoff, .backoff_ms = rotation_backoff_ms_};
    }
    // A short or failed write still owns its suffix. Rotation waits until that span finishes.
    if (pending_length_ != 0) return std::unexpected(ErrorCode::kConsumerBusy);
    if (rotation.ReopenPending()) {
      SetIo(flags, zerolog::abi::WriteRotateState::kOpening);
      return IoAction{.step = IoStep::kOpenNew};
    }
    if (!rotation.OldestUnlinked()) {
      SetIo(flags, zerolog::abi::WriteRotateState::kRenaming);
      SetRenameStep(flags, static_cast<std::uint8_t>(max_files_ - 1));
      return IoAction{.step = IoStep::kUnlinkOldest};
    }
    const auto slot = rotation.NextRenameSlot();
    if (slot >= max_files_) return std::unexpected(ErrorCode::kInvalidArgument);
    SetIo(flags, zerolog::abi::WriteRotateState::kRenaming);
    SetRenameStep(flags, slot);
    return IoAction{.step = IoStep::kRename, .rename_slot = slot};
  }

  /** @brief Record the unlink completion on `rotation` and either rename or arm the rotation window. */
  template <class Rotation>
  [[nodiscard]] Result<IoAction> OnUnlinkCqe(zerolog::abi::SinkFlagsWord& flags, Rotation& rotation,
                                             std::int32_t res) noexcept {
    if (IoOf(flags) != zerolog::abi::WriteRotateState::kRenaming) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    const auto done = rotation.OnUnlinkOldestDone(res);
    if (!done) {
      ArmRotation(flags);
      return std::unexpected(done.error());
    }
    const auto slot = rotation.NextRenameSlot();
    SetRenameStep(flags, slot);
    return IoAction{.step = IoStep::kRename, .rename_slot = slot};
  }

  /** @brief Record the rename completion. Slot `0` moves the machine to `kOpening`. */
  template <class Rotation>
  [[nodiscard]] Result<IoAction> OnRenameCqe(zerolog::abi::SinkFlagsWord& flags, Rotation& rotation,
                                             std::int32_t res) noexcept {
    if (IoOf(flags) != zerolog::abi::WriteRotateState::kRenaming) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    const auto slot = RenameStep(flags);
    const auto done = rotation.OnRenameStepDone(slot, res);
    if (!done) {
      ArmRotation(flags);
      return std::unexpected(done.error());
    }
    if (rotation.ReopenPending()) {
      SetIo(flags, zerolog::abi::WriteRotateState::kOpening);
      return IoAction{.step = IoStep::kOpenNew};
    }
    const auto next = rotation.NextRenameSlot();
    SetRenameStep(flags, next);
    return IoAction{.step = IoStep::kRename, .rename_slot = next};
  }

  /**
   * @brief Record the replacement `openat`.
   * Failure keeps the rotation cursor on the open and returns to `kReady` so writes can continue.
   * Success transfers the previous descriptor through `IoAction::close_fd`.
   */
  template <class Rotation>
  [[nodiscard]] Result<IoAction> OnOpenCqe(zerolog::abi::SinkFlagsWord& flags, Rotation& rotation, std::int32_t res) noexcept {
    if (IoOf(flags) != zerolog::abi::WriteRotateState::kOpening) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    auto opened = rotation.OnOpenNewDone(res);
    if (!opened) {
      ArmRotation(flags);
      return std::unexpected(opened.error());
    }
    const auto previous = opened->release();
    if (previous < 0) {
      FinishRotation(flags);
      return IoAction{};
    }
    SetIo(flags, zerolog::abi::WriteRotateState::kClosingOld);
    SetRotationRetry(flags, false);
    return IoAction{.step = IoStep::kCloseOld, .close_fd = previous};
  }

  /** @brief The previous descriptor is closed. `-EINTR` and `-EBADF` still retire the step. */
  [[nodiscard]] Result<IoAction> OnCloseCqe(zerolog::abi::SinkFlagsWord& flags, std::int32_t res) noexcept {
    if (flags.get<^^zerolog::abi::SinkFlagsSpec::io>() != zerolog::abi::WriteRotateState::kClosingOld) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (res < 0 && res != -EINTR && res != -EBADF) return std::unexpected(ErrorCode::kRotateFailed);
    FinishRotation(flags);
    return IoAction{};
  }

  /**
   * @brief A backoff window elapsed.
   * `write_window` selects the write window; otherwise the rotation window resumes from the shared cursor.
   */
  template <class Rotation>
  [[nodiscard]] Result<IoAction> OnBackoffElapsed(zerolog::abi::SinkFlagsWord& flags, Rotation& rotation,
                                                  bool write_window) noexcept {
    if (write_window) {
      if (!WriteRetry(flags)) return std::unexpected(ErrorCode::kInvalidArgument);
      SetWriteRetry(flags, false);
      if (pending_length_ == 0) {
        SetIo(flags, zerolog::abi::WriteRotateState::kReady);
        return IoAction{};
      }
      SetIo(flags, zerolog::abi::WriteRotateState::kWriting);
      return IoAction{.step = IoStep::kWrite, .length = pending_length_};
    }
    if (!RotationRetry(flags)) return std::unexpected(ErrorCode::kInvalidArgument);
    SetRotationRetry(flags, false);
    return BeginRotation(flags, rotation);
  }

  /** @brief Milliseconds until the write window elapses, or zero when it is clear. */
  [[nodiscard]] std::uint32_t WriteBackoffMs() const noexcept { return write_backoff_ms_; }
  /** @brief Milliseconds until the rotation window elapses, or zero when it is clear. */
  [[nodiscard]] std::uint32_t RotationBackoffMs() const noexcept { return rotation_backoff_ms_; }
  /** @brief Bytes of the current span that have not reached a full completion. */
  [[nodiscard]] std::uint32_t PendingLength() const noexcept { return pending_length_; }

  /** @brief Roll back an unsubmitted step when staging an SQE into the Proactor fails. */
  void RollbackUnsubmitted(zerolog::abi::SinkFlagsWord& flags, bool write_step) noexcept {
    if (write_step) {
      ArmWrite(flags);
    } else {
      ArmRotation(flags);
    }
  }

  /** @brief Retire async I/O state to `kReady` when stopping so synchronous shutdown flush can take over. */
  static void YieldForShutdown(zerolog::abi::SinkFlagsWord& flags) noexcept {
    SetIo(flags, zerolog::abi::WriteRotateState::kReady);
  }

  /** @brief Clear `pending_length_` and retry state after synchronous shutdown flush drains the frozen suffix. */
  void CompleteSyncFlush(zerolog::abi::SinkFlagsWord& flags) noexcept {
    pending_length_ = 0;
    write_shift_ = 0;
    write_backoff_ms_ = 0;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::write_retry>(false).with<^^zerolog::abi::SinkFlagsSpec::io>(
        zerolog::abi::WriteRotateState::kReady);
  }

  /** @brief Clear rotation retry state after synchronous shutdown rotation finishes. */
  void CompleteSyncRotation(zerolog::abi::SinkFlagsWord& flags) noexcept { FinishRotation(flags); }

 private:
  [[nodiscard]] static bool Stopped(const zerolog::abi::SinkFlagsWord& flags) noexcept {
    const auto life = flags.get<^^zerolog::abi::SinkFlagsSpec::lifecycle>();
    return life == zerolog::abi::LifecycleState::kStopping || life == zerolog::abi::LifecycleState::kDetached ||
           life == zerolog::abi::LifecycleState::kClosed;
  }

  [[nodiscard]] static bool Busy(const zerolog::abi::SinkFlagsWord& flags) noexcept {
    const auto io = flags.get<^^zerolog::abi::SinkFlagsSpec::io>();
    return io == zerolog::abi::WriteRotateState::kWriting || io == zerolog::abi::WriteRotateState::kRenaming ||
           io == zerolog::abi::WriteRotateState::kOpening || io == zerolog::abi::WriteRotateState::kClosingOld;
  }

  [[nodiscard]] static zerolog::abi::WriteRotateState IoOf([[maybe_unused]] const zerolog::abi::SinkFlagsWord& flags) noexcept {
    return flags.get<^^zerolog::abi::SinkFlagsSpec::io>();
  }

  [[nodiscard]] static std::uint8_t RenameStep([[maybe_unused]] const zerolog::abi::SinkFlagsWord& flags) noexcept {
    return flags.get<^^zerolog::abi::SinkFlagsSpec::rename_step>();
  }

  [[nodiscard]] static bool WriteRetry([[maybe_unused]] const zerolog::abi::SinkFlagsWord& flags) noexcept {
    return flags.test<^^zerolog::abi::SinkFlagsSpec::write_retry>();
  }

  [[nodiscard]] static bool RotationRetry([[maybe_unused]] const zerolog::abi::SinkFlagsWord& flags) noexcept {
    return flags.test<^^zerolog::abi::SinkFlagsSpec::rotation_retry>();
  }

  static void SetIo([[maybe_unused]] zerolog::abi::SinkFlagsWord& flags,
                    [[maybe_unused]] zerolog::abi::WriteRotateState state) noexcept {
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::io>(state);
  }

  static void SetRenameStep([[maybe_unused]] zerolog::abi::SinkFlagsWord& flags, [[maybe_unused]] std::uint8_t step) noexcept {
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::rename_step>(step);
  }

  static void SetWriteRetry([[maybe_unused]] zerolog::abi::SinkFlagsWord& flags, [[maybe_unused]] bool armed) noexcept {
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::write_retry>(armed);
  }

  static void SetRotationRetry([[maybe_unused]] zerolog::abi::SinkFlagsWord& flags, [[maybe_unused]] bool armed) noexcept {
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::rotation_retry>(armed);
  }

  void FinishRotation(zerolog::abi::SinkFlagsWord& flags) noexcept {
    rotation_shift_ = 0;
    rotation_backoff_ms_ = 0;
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::io>(zerolog::abi::WriteRotateState::kReady)
                .with<^^zerolog::abi::SinkFlagsSpec::rotation_retry>(false)
                .with<^^zerolog::abi::SinkFlagsSpec::rename_step>(static_cast<std::uint8_t>(0));
  }

  void ArmWrite(zerolog::abi::SinkFlagsWord& flags) noexcept {
    write_backoff_ms_ = detail::BumpBackoffMs(write_shift_);
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::write_retry>(true).with<^^zerolog::abi::SinkFlagsSpec::io>(
        zerolog::abi::WriteRotateState::kReady);
  }

  void ArmRotation(zerolog::abi::SinkFlagsWord& flags) noexcept {
    rotation_backoff_ms_ = detail::BumpBackoffMs(rotation_shift_);
    flags = flags.with<^^zerolog::abi::SinkFlagsSpec::rotation_retry>(true).with<^^zerolog::abi::SinkFlagsSpec::io>(
        zerolog::abi::WriteRotateState::kReady);
  }

  std::uint32_t pending_length_{};       ///< Suffix still owed by the current span.
  std::uint32_t write_backoff_ms_{};     ///< Active write window, or zero.
  std::uint32_t rotation_backoff_ms_{};  ///< Active rotation window, or zero.
  std::uint8_t max_files_{};             ///< Bound archive depth.
  std::uint8_t write_shift_{};           ///< Next write delay is `1 << write_shift_` milliseconds.
  std::uint8_t rotation_shift_{};        ///< Next rotation delay is `1 << rotation_shift_` milliseconds.
  std::uint8_t bound_{};                 ///< `Bind` accepted a depth.
};

static_assert(uarch::NoPadding<WriteRotateFsm>);

}  // namespace zerolog::sink

namespace zerolog {
using sink::IoAction;
using sink::IoStep;
using sink::WriteRotateFsm;
}  // namespace zerolog
