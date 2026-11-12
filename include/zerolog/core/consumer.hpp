/** @file @brief Exclusive consumer handles and synchronous polling over persistent merge/output state. */
#pragma once
#include <memory>
#include <zerolog/core/consumer_core.hpp>
#include <zerolog/core/lease.hpp>
namespace zerolog::core {
/** @brief The lease protects initialization and all accesses; pending bytes survive owner handoffs. */
union ConsumerStorage {
  char dummy{};
  ConsumerCore value;
  constexpr ConsumerStorage() noexcept : dummy{} {}
  // Late static destructors may still log or poll; process teardown owns these mappings.
  ~ConsumerStorage() noexcept {}
};
inline constinit ConsumerStorage g_consumer_core;
inline constinit bool g_consumer_initialized = false;
/** @brief A move-only right to operate on the process consumer's stable storage. */
class LogConsumer final {
 public:
  LogConsumer(const LogConsumer&) = delete;
  LogConsumer& operator=(const LogConsumer&) = delete;
  LogConsumer(LogConsumer&& other) noexcept : lease_(std::move(other.lease_)), core_(std::exchange(other.core_, nullptr)) {}
  LogConsumer& operator=(LogConsumer&&) = delete;
  /** @brief Initialize once under a valid lease; an invalid handle never accesses shared state. */
  [[nodiscard]] static Result<LogConsumer> Create(ConsumerLease lease) noexcept {
    if (!lease.Valid() || ConsumerReentry()) return std::unexpected(ErrorCode::kConsumerBusy);
    if (!g_consumer_initialized) {
      auto created = ConsumerCore::Create();
      if (!created) return std::unexpected(created.error());
      std::construct_at(&g_consumer_core.value, std::move(*created));
      g_consumer_initialized = true;
    }
    return LogConsumer{std::move(lease), &g_consumer_core.value};
  }
  /** @brief Format without calling a synchronous writer; external proactors use this operation. */
  [[nodiscard]] Result<abi::DrainStats> DrainBatch(abi::DrainBudget budget = {}) noexcept {
    if (core_ == nullptr || !lease_.Valid()) return std::unexpected(ErrorCode::kConsumerBusy);
    return core_->DrainBatch(sys::ReadTsc(), budget);
  }
  Result<abi::DrainStats> Poll(bool force_flush = false) noexcept { return PollBatch({}, force_flush); }
  /** @brief Mode C returns decode and write errors directly, retaining retryable output for the next owner. */
  Result<abi::DrainStats> PollBatch(abi::DrainBudget budget, bool force_flush = false) noexcept {
    auto result = DrainBatch(budget);
    if (!result) return result;
    if (force_flush || core_->Output().TotalUnflushedBytes() != 0) {
      if (auto flushed = core_->FlushSync(); !flushed) return std::unexpected(flushed.error());
    }
    return result;
  }
  ErrorCode SetWriter(WriteCallback writer) noexcept {
    if (core_ == nullptr || !lease_.Valid() || ConsumerReentry()) return ErrorCode::kConsumerBusy;
    return core_->SetWriter(writer);
  }
  ErrorCode SetOutputFd(int fd) noexcept {
    if (core_ == nullptr || !lease_.Valid() || ConsumerReentry()) return ErrorCode::kConsumerBusy;
    return core_->SetOutputFd(fd);
  }
  /** @brief Borrow consumer state for an external sink while retaining this handle's lease. */
  [[nodiscard]] Result<ConsumerCore*> Core() noexcept {
    if (core_ == nullptr || !lease_.Valid()) return std::unexpected(ErrorCode::kConsumerBusy);
    return core_;
  }

 private:
  LogConsumer(ConsumerLease lease, ConsumerCore* core) noexcept : lease_(std::move(lease)), core_(core) {}
  ConsumerLease lease_;
  ConsumerCore* core_;
};
/** @brief Acquire the process consumer for an external sink. */
[[nodiscard]] inline Result<LogConsumer> AcquireConsumer() noexcept {
  if (ConsumerReentry()) return std::unexpected(ErrorCode::kConsumerBusy);
  auto lease = TryAcquireExternalLease();
  if (!lease) return std::unexpected(lease.error());
  return LogConsumer::Create(std::move(*lease));
}
/** @brief Acquire a temporary Mode C lease and return the exact Result<DrainStats>. */
[[nodiscard]] inline Result<abi::DrainStats> TryPollBatch(abi::DrainBudget budget, bool force_flush = false) noexcept {
  if (ConsumerReentry()) return std::unexpected(ErrorCode::kConsumerBusy);
  auto lease = TryStartPoller();
  if (!lease) return std::unexpected(lease.error());
  auto consumer = LogConsumer::Create(std::move(*lease));
  if (!consumer) return std::unexpected(consumer.error());
  return consumer->PollBatch(budget, force_flush);
}
[[nodiscard]] inline Result<abi::DrainStats> TryPoll(bool force_flush = false) noexcept {
  return TryPollBatch({}, force_flush);
}
/** @brief Acquire a temporary Mode C lease, preserving output and heap state between polls. */
inline Result<abi::DrainStats> PollBatch(abi::DrainBudget budget, bool force_flush = false) noexcept {
  return TryPollBatch(budget, force_flush);
}
inline Result<abi::DrainStats> Poll(bool force_flush = false) noexcept { return TryPollBatch({}, force_flush); }
}  // namespace zerolog::core
namespace zerolog {
using core::AcquireConsumer;
using core::LogConsumer;
using core::Poll;
using core::PollBatch;
using core::TryPoll;
using core::TryPollBatch;
using core::WriteCallback;
}  // namespace zerolog
