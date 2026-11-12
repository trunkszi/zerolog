/** @file @brief Budgeted timestamp merge, retained queue heads and explicit synchronous target writes. */
#pragma once
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <span>
#include <uarch/core/flow/delegate.hpp>
#include <zerolog/core/registry.hpp>
#include <zerolog/core/scratch_heap.hpp>
#include <zerolog/core/thread_buffer.hpp>
#include <zerolog/format/output_buffer.hpp>
#include <zerolog/sys/tsc_clock.hpp>
namespace zerolog::core {
/** @brief A synchronous target acknowledges only the returned prefix length. */
using WriteCallback = uarch::Callback<Result<std::uint32_t>(std::span<const char>)>;
/** @brief Deterministic merge state, with no implicit writes during DrainBatch. */
class ConsumerCore final {
 public:
  ConsumerCore(const ConsumerCore&) = delete;
  ConsumerCore& operator=(const ConsumerCore&) = delete;
  ConsumerCore(ConsumerCore&&) noexcept = default;
  ConsumerCore& operator=(ConsumerCore&&) = delete;
  /** @brief Establish output mappings and the clock before accepting any queue records. */
  [[nodiscard]] static Result<ConsumerCore> Create(ThreadRegistry& registry = GlobalRegistry()) noexcept {
    auto output = format::OutputBuffer::Create();
    if (!output) return std::unexpected(output.error());
    auto clock = sys::TscClock::Calibrate();
    if (!clock) return std::unexpected(clock.error());
    return ConsumerCore{registry, std::move(*output), std::move(*clock)};
  }
  /** @brief Merge up to the record limit and a soft byte yield threshold. Errors discard exactly the offending record, never
   * earlier output. */
  [[nodiscard]] Result<abi::DrainStats> DrainBatch(std::uint64_t now_tsc, abi::DrainBudget budget = {}) noexcept {
    if (ConsumerReentry()) return std::unexpected(ErrorCode::kConsumerBusy);
    ScopedReentrancyFlag guard{kInConsumer};
    if (auto calibrated = clock_.Calibrate(now_tsc); !calibrated) return std::unexpected(calibrated.error());
    if (const auto error = Fill(); error != ErrorCode::kSuccess) return std::unexpected(error);
    abi::DrainStats stats{};
    abi::DrainFlagsWord flags{};
    while (!heap_.Empty()) {
      if (stats.records >= budget.max_records || (stats.records != 0 && stats.bytes >= budget.max_bytes)) {
        flags = flags.with<^^abi::DrainFlagsSpec::budget_exhausted>(true);
        break;
      }
      auto popped = heap_.Pop();
      if (!popped) return std::unexpected(popped.error());
      const auto node = *popped;
      auto* buffer = registry_->GetSlotBuffer(node.slot);
      if (buffer == nullptr) return std::unexpected(ErrorCode::kInvalidArgument);
      const auto view = buffer->queue.Front();
      if (!view.Valid() || node.blocks != view.header.get<^^abi::RecordHeaderSpec::blocks>() || node.blocks < 4 ||
          node.blocks > abi::kMaxRecordBlocks) {
        DiscardCorruptedHead(*buffer, node.blocks);
        return std::unexpected(ErrorCode::kInvalidArgument);
      }
      abi::RecordPrefix prefix{};
      std::memcpy(&prefix, view.data, sizeof(prefix));
      const auto payload = view.header.get<^^abi::RecordHeaderSpec::payload_bytes>();
      const auto wall = clock_.ToWallNanoseconds(prefix.tsc);
      ErrorCode error = ErrorCode::kInvalidArgument;
      const auto start = output_.PendingBytes();
      if (payload <= node.blocks * abi::Block::value - sizeof(prefix) &&
          wall <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        const auto name = buffer->ReadThreadName();
        const auto name_view = name.View();
        // The registry slot is the protocol thread ID; formatting must not query an OS TID.
        error = output_.FormatRecord(prefix, {view.data + sizeof(prefix), payload, 0}, static_cast<std::int64_t>(wall),
                                     node.slot, node.slot, name_view);
      }
      if (error == ErrorCode::kWouldBlock) {
        const auto restored = heap_.TryPush(node);
        if (restored != ErrorCode::kSuccess) return std::unexpected(restored);
        flags = flags.with<^^abi::DrainFlagsSpec::backpressure>(true);
        break;
      }
      if (error != ErrorCode::kSuccess) {
        buffer->queue.Pop(node.blocks);
        return std::unexpected(error);
      }
      const auto bytes = output_.PendingBytes() - start;
      if (stats.records != 0 && bytes > budget.max_bytes - stats.bytes) {
        if (const auto rollback = output_.Rollback(start); rollback != ErrorCode::kSuccess) return std::unexpected(rollback);
        if (const auto restored = heap_.TryPush(node); restored != ErrorCode::kSuccess) return std::unexpected(restored);
        flags = flags.with<^^abi::DrainFlagsSpec::budget_exhausted>(true);
        break;
      }
      ++stats.records;
      stats.bytes += static_cast<std::uint32_t>(bytes);
      if (view.header.get<^^abi::RecordHeaderSpec::level>() >= GetFlushLevel())
        flags = flags.with<^^abi::DrainFlagsSpec::urgent_seen>(true);
      buffer->queue.Pop(node.blocks);
      if (const auto next = QueueHead(node.slot); next != ErrorCode::kSuccess) return std::unexpected(next);
      if (stats.bytes >= budget.max_bytes || stats.records >= budget.max_records) {
        flags = flags.with<^^abi::DrainFlagsSpec::budget_exhausted>(true);
        break;
      }
    }
    flags = flags.with<^^abi::DrainFlagsSpec::more_pending>(!heap_.Empty())
                .with<^^abi::DrainFlagsSpec::frozen_in_flight>(output_.FrozenBytes() != 0);
    stats.flags = flags.raw();
    return stats;
  }
  /** @brief Synchronous Mode C writes exclusively to the explicitly configured callback or fd. */
  [[nodiscard]] Result<void> FlushSync() noexcept {
    if (ConsumerReentry()) return std::unexpected(ErrorCode::kConsumerBusy);
    ScopedReentrancyFlag guard{kInConsumer};
    if (output_.FrozenBytes() != 0) return std::unexpected(ErrorCode::kWouldBlock);
    auto slab = output_.Freeze();
    while (slab) {
      const std::span<const char> bytes{slab->data + slab->offset, slab->length - slab->offset};
      auto written = Write(bytes);
      ErrorCode error = ErrorCode::kSuccess;
      if (!written)
        error = written.error();
      else if (*written == 0)
        error = ErrorCode::kWriteFailed;
      else if (*written > bytes.size())
        error = ErrorCode::kInvalidArgument;
      if (error != ErrorCode::kSuccess) {
        const auto restored = output_.RestoreFailed(*slab);
        return std::unexpected(restored == ErrorCode::kSuccess ? error : restored);
      }
      auto completed = output_.CompleteFrozen(*slab, *written);
      if (!completed) return std::unexpected(completed.error());
      slab = *completed;
    }
    return {};
  }
  [[nodiscard]] ErrorCode SetWriter(WriteCallback writer) noexcept {
    if (!writer) return ErrorCode::kInvalidArgument;
    writer_ = writer;
    return ErrorCode::kSuccess;
  }
  [[nodiscard]] ErrorCode SetOutputFd(int fd) noexcept {
    if (fd < 0) return ErrorCode::kInvalidArgument;
    fd_ = fd;
    writer_.reset();
    return ErrorCode::kSuccess;
  }
  [[nodiscard]] format::OutputBuffer& Output() noexcept { return output_; }
  [[nodiscard]] const format::OutputBuffer& Output() const noexcept { return output_; }
  [[nodiscard]] std::size_t RetainedHeads() const noexcept { return heap_.Size(); }

 private:
  ConsumerCore(ThreadRegistry& registry, format::OutputBuffer output, sys::TscClock clock) noexcept
      : registry_(&registry), output_(std::move(output)), clock_(std::move(clock)) {}
  static void DiscardCorruptedHead(ThreadBuffer& buffer, std::uint32_t blocks) noexcept {
    // Bound the scrub to the mapped tail; never skip an unpublished, zero header.
    const auto view = buffer.queue.Front();
    if (view.data == nullptr) return;
    const auto tail = SpscVarQueue::kRingCap - buffer.queue.ConsumerIdx();
    const auto extent = std::clamp(blocks, 4U, abi::kMaxRecordBlocks);
    if (extent < tail) {
      buffer.queue.Pop(extent);
    } else {
      auto* header = reinterpret_cast<std::uint64_t*>(const_cast<std::byte*>(view.data));
      const auto wrap = abi::RecordHeaderWord{}.with<^^abi::RecordHeaderSpec::blocks>(abi::kWrapSentinel);
      std::atomic_ref<std::uint64_t>{*header}.store(wrap.raw(), std::memory_order_relaxed);
      (void)buffer.queue.Front();
    }
  }
  [[nodiscard]] ErrorCode QueueHead(std::uint32_t slot) noexcept {
    if (heap_.Contains(slot)) return ErrorCode::kSuccess;
    auto* buffer = registry_->GetSlotBuffer(slot);
    if (buffer == nullptr) return ErrorCode::kInvalidArgument;
    const auto view = buffer->queue.Front();
    if (view.data != nullptr) {
      const auto blocks = view.header.get<^^abi::RecordHeaderSpec::blocks>();
      if (!view.Valid() || blocks > abi::kMaxRecordBlocks) {
        DiscardCorruptedHead(*buffer, blocks);
        return ErrorCode::kInvalidArgument;
      }
      abi::RecordPrefix prefix{};
      std::memcpy(&prefix, view.data, sizeof(prefix));
      return heap_.TryPush({prefix.tsc, slot, view.header.get<^^abi::RecordHeaderSpec::blocks>()});
    }
    (void)registry_->TryReclaimOrphanedSlot(slot);
    return ErrorCode::kSuccess;
  }
  [[nodiscard]] ErrorCode Fill() noexcept {
    auto slots = registry_->ActiveSlotsBitmap();
    while (auto slot = slots.pop_lowest_one()) {
      if (const auto error = QueueHead(*slot); error != ErrorCode::kSuccess) return error;
    }
    return ErrorCode::kSuccess;
  }
  [[nodiscard]] Result<std::uint32_t> Write(std::span<const char> bytes) noexcept {
    if (writer_) return writer_(bytes);
    for (;;) {
      const auto count = ::write(fd_, bytes.data(), bytes.size());
      if (count >= 0) return static_cast<std::uint32_t>(count);
      if (errno == EINTR) continue;
      return std::unexpected(errno == EAGAIN ? ErrorCode::kWouldBlock : ErrorCode::kWriteFailed);
    }
  }
  ThreadRegistry* registry_; /**< Registry lifetime exceeds this consumer. */
  format::OutputBuffer output_;
  sys::TscClock clock_;
  ScratchHeap heap_;
  WriteCallback writer_{};
  int fd_{2};
};
}  // namespace zerolog::core
