/** @file @brief Mode B standalone consumer worker thread with 512 KiB stack and automatic futex2 fallback. */
#pragma once

#include <pthread.h>

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <uarch/uarch.hpp>
#include <uring/caps/kernel_caps.hpp>
#include <uring/core/dispatcher.hpp>
#include <uring/core/proactor.hpp>
#include <uring/error/uring_error.hpp>
#include <utility>
#include <zerolog/core/consumer.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/lease.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/sink/futex_fallback.hpp>
#include <zerolog/sink/proactor_sink.hpp>
#include <zerolog/sink/rotation.hpp>

namespace zerolog::sink {

/** @brief Active I/O backend selected by `UringConsumerThread` at worker startup. */
enum class WorkerBackend : std::uint8_t {
  kNone = 0,
  kIoUring = 1,
  kFutexFallback = 2,
};

/** @brief Configuration for `UringConsumerThread::Start`. */
struct WorkerConfig {
  std::string_view base_path{};                      ///< Active log file path (`base`), or empty when `output_fd >= 0`.
  RotationPolicy rotation{};                         ///< Size threshold and archive retention policy.
  SinkConfig sink{};                                 ///< Drain budget and idle flush interval.
  std::uint32_t ring_entries{64U};                   ///< `io_uring` SQ depth (power of two).
  std::optional<uring::UringErrorCode> fault_uring;  ///< Optional deterministic test fault injection.
  std::int32_t output_fd{-1};                        ///< Stream descriptor (`STDERR_FILENO`, pipe) when `base_path` is empty.
};

/**
 * @brief Mode B standalone consumer worker thread.
 *
 * Spawns a dedicated `pthread` with a `512 KiB` stack (`kWorkerStackBytes`), acquires the exclusive
 * internal poller lease (`TryStartPoller` + `CommitPollerStart`), constructs both `RotationType` and
 * `uring::Proactor` in place on the 512 KiB worker frame (keeping `sizeof(UringConsumerThread)` small enough
 * for 128 KiB musl caller stacks), and falls back to `FutexFallbackSink` when `io_uring` is unavailable
 * (`ErrorCode::kProactorUnavailable`).
 */
template <class RotationType = Rotation>
class UringConsumerThread final {
 public:
  static constexpr std::size_t kWorkerStackBytes = 512U * 1024U;

  UringConsumerThread() noexcept = default;
  UringConsumerThread(const UringConsumerThread&) = delete ("UringConsumerThread owns a live worker thread");
  UringConsumerThread& operator=(const UringConsumerThread&) = delete ("UringConsumerThread owns a live worker thread");
  UringConsumerThread(UringConsumerThread&&) = delete ("UringConsumerThread owns a live worker thread");
  UringConsumerThread& operator=(UringConsumerThread&&) = delete ("UringConsumerThread owns a live worker thread");

  ~UringConsumerThread() noexcept {
    if (running_.load(std::memory_order_acquire)) {
      [[maybe_unused]] const auto stopped = Stop();
    }
  }

  /**
   * @brief Start the Mode B consumer worker with explicit `WorkerConfig`.
   */
  [[nodiscard]] Result<void> Start(WorkerConfig config) noexcept {
    const bool has_path = !config.base_path.empty();
    if (uarch::unlikely((!has_path && config.output_fd < 0) ||
                        (has_path && (config.base_path.size() + 4U >= RotationType::kPathCap ||
                                      config.base_path.find('\0') != std::string_view::npos ||
                                      config.rotation.max_files == 0U || config.rotation.max_files > 15U)) ||
                        config.ring_entries == 0U || !std::has_single_bit(config.ring_entries))) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    bool expected_running = false;
    if (!running_.compare_exchange_strong(expected_running, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return std::unexpected(ErrorCode::kConsumerBusy);
    }

    auto lease = core::TryStartPoller();
    if (!lease) {
      running_.store(false, std::memory_order_release);
      return std::unexpected(lease.error());
    }
    if (auto committed = core::CommitPollerStart(*lease); !committed) {
      running_.store(false, std::memory_order_release);
      return std::unexpected(committed.error());
    }

    auto consumer = core::LogConsumer::Create(std::move(*lease));
    if (!consumer) {
      running_.store(false, std::memory_order_release);
      return std::unexpected(consumer.error());
    }

    if (!config.base_path.empty()) {
      std::memcpy(base_path_buf_.data(), config.base_path.data(), config.base_path.size());
    }
    base_path_buf_[config.base_path.size()] = '\0';
    base_path_len_ = config.base_path.size();
    config_ = config;
    config_.base_path = std::string_view{base_path_buf_.data(), base_path_len_};
    consumer_.emplace(std::move(*consumer));
    stop_requested_.store(false, std::memory_order_relaxed);
    startup_state_.store(0U, std::memory_order_relaxed);
    startup_error_ = ErrorCode::kSuccess;
    worker_error_ = ErrorCode::kSuccess;
    backend_.store(WorkerBackend::kNone, std::memory_order_relaxed);
    worker_stack_size_.store(0U, std::memory_order_relaxed);

    pthread_attr_t attr{};
    if (::pthread_attr_init(&attr) != 0) {
      consumer_.reset();
      running_.store(false, std::memory_order_release);
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (::pthread_attr_setstacksize(&attr, kWorkerStackBytes) != 0) {
      ::pthread_attr_destroy(&attr);
      consumer_.reset();
      running_.store(false, std::memory_order_release);
      return std::unexpected(ErrorCode::kInvalidArgument);
    }

    const int created = ::pthread_create(&thread_, &attr, &UringConsumerThread::ThreadEntry, this);
    ::pthread_attr_destroy(&attr);
    if (created != 0) {
      consumer_.reset();
      running_.store(false, std::memory_order_release);
      return std::unexpected(created == EAGAIN ? ErrorCode::kTooManyThreads : ErrorCode::kInvalidArgument);
    }

    while (startup_state_.load(std::memory_order_acquire) == 0U) {
      __builtin_ia32_pause();
    }

    if (startup_state_.load(std::memory_order_acquire) == 2U) {
      (void)::pthread_join(thread_, nullptr);
      consumer_.reset();
      running_.store(false, std::memory_order_release);
      return std::unexpected(startup_error_);
    }

    return {};
  }

  /** @brief Convenience overload starting the worker with `base_path`, `RotationPolicy`, and `SinkConfig`. */
  [[nodiscard]] Result<void> Start(std::string_view base_path, RotationPolicy policy = {},
                                   SinkConfig sink_config = {}) noexcept {
    return Start(WorkerConfig{
        .base_path = base_path,
        .rotation = policy,
        .sink = sink_config,
        .ring_entries = 64U,
        .fault_uring = std::nullopt,
    });
  }

  /**
   * @brief Signal the worker thread to stop, wake the doorbell, join the thread, and return any flush error.
   */
  [[nodiscard]] Result<void> Stop() noexcept {
    if (!running_.load(std::memory_order_acquire)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (::pthread_equal(::pthread_self(), thread_) != 0) {
      return std::unexpected(ErrorCode::kConsumerBusy);
    }

    stop_requested_.store(true, std::memory_order_release);
    core::RingDoorbellUrgentCold();

    const int joined = ::pthread_join(thread_, nullptr);
    consumer_.reset();
    running_.store(false, std::memory_order_release);

    if (joined != 0) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (worker_error_ != ErrorCode::kSuccess) {
      return std::unexpected(worker_error_);
    }
    return {};
  }

  [[nodiscard]] bool Running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] WorkerBackend Backend() const noexcept { return backend_.load(std::memory_order_acquire); }
  [[nodiscard]] std::size_t WorkerStackSize() const noexcept { return worker_stack_size_.load(std::memory_order_acquire); }

 private:
  using SinkType = ProactorLogSink<uring::Proactor, 1, RotationType>;

  static void* ThreadEntry(void* arg) noexcept {
    static_cast<UringConsumerThread*>(arg)->ThreadMain();
    return nullptr;
  }

  void RecordObservedStackSize() noexcept {
    pthread_attr_t attr{};
    if (::pthread_getattr_np(::pthread_self(), &attr) == 0) {
      std::size_t stack_bytes = 0U;
      if (::pthread_attr_getstacksize(&attr, &stack_bytes) == 0) {
        worker_stack_size_.store(stack_bytes, std::memory_order_release);
      }
      ::pthread_attr_destroy(&attr);
    }
  }

  void ThreadMain() noexcept {
    RecordObservedStackSize();

    auto core_res = consumer_->Core();
    if (!core_res) {
      startup_error_ = core_res.error();
      startup_state_.store(2U, std::memory_order_release);
      return;
    }
    core::ConsumerCore& core = **core_res;

    RotationType rotation{};
    const auto opened =
        config_.base_path.empty() ? rotation.OpenFd(config_.output_fd) : rotation.Open(config_.base_path, config_.rotation);
    if (!opened) {
      startup_error_ = opened.error();
      startup_state_.store(2U, std::memory_order_release);
      return;
    }

    if (!config_.fault_uring.has_value()) {
      auto caps = uring::caps::KernelCaps::Probe(SinkType::RequiredCaps());
      if (!caps) {
        const ErrorCode err = FromUring(caps.error());
        if (err == ErrorCode::kProactorUnavailable) {
          RunFallbackOnStack(core, rotation);
          return;
        }
        startup_error_ = err;
        startup_state_.store(2U, std::memory_order_release);
        return;
      }

      auto built = uring::Proactor::Builder().Entries(config_.ring_entries).Build();
      if (!built) {
        const ErrorCode err = FromUring(built.error());
        if (err == ErrorCode::kProactorUnavailable) {
          RunFallbackOnStack(core, rotation);
          return;
        }
        startup_error_ = err;
        startup_state_.store(2U, std::memory_order_release);
        return;
      }

      RunProactorLoop(*built, core, rotation);
      return;
    }

    const ErrorCode fault_err = FromUring(*config_.fault_uring);
    if (fault_err == ErrorCode::kProactorUnavailable) {
      RunFallbackOnStack(core, rotation);
      return;
    }
    startup_error_ = fault_err;
    startup_state_.store(2U, std::memory_order_release);
  }

  void RunProactorLoop(uring::Proactor& engine, core::ConsumerCore& core, RotationType& rotation) noexcept {
    SinkType sink{engine, core, rotation, config_.sink};
    uring::SingleDispatcher<SinkType> dispatcher{sink};
    engine.BindDispatcher(dispatcher);

    if (auto started = sink.Start(); !started) {
      if (started.error() == ErrorCode::kProactorUnavailable) {
        [[maybe_unused]] const auto req = sink.RequestStop();
        while (engine.Stage() != uring::DrainStage::kClosed) {
          if (engine.ShutdownWith(dispatcher, std::chrono::milliseconds(50))) break;
        }
        RunFallbackOnStack(core, rotation);
        return;
      }
      [[maybe_unused]] const auto req = sink.RequestStop();
      while (engine.Stage() != uring::DrainStage::kClosed) {
        if (engine.ShutdownWith(dispatcher, std::chrono::milliseconds(50))) break;
      }
      startup_error_ = started.error();
      startup_state_.store(2U, std::memory_order_release);
      return;
    }

    backend_.store(WorkerBackend::kIoUring, std::memory_order_release);
    startup_state_.store(1U, std::memory_order_release);

    while (!stop_requested_.load(std::memory_order_acquire)) {
      if (engine.InFlight() == 0U && engine.DeferredCount() == 0U) {
        if (auto serviced = sink.Service(); !serviced) {
          worker_error_ = serviced.error();
          break;
        }
      }
      if (stop_requested_.load(std::memory_order_acquire)) {
        break;
      }
      const auto wait_res = engine.WaitCompletions(1U, std::nullopt, dispatcher);
      if (!wait_res && wait_res.error() != uring::UringErrorCode::kTimedOut &&
          wait_res.error() != uring::UringErrorCode::kInterrupted && wait_res.error() != uring::UringErrorCode::kAgain) {
        worker_error_ = FromUring(wait_res.error());
        break;
      }
    }

    if (auto req = sink.RequestStop(); !req && worker_error_ == ErrorCode::kSuccess) {
      worker_error_ = req.error();
    }
    for (std::size_t spin = 0U; spin < 4096U && !sink.Quiescent(); ++spin) {
      [[maybe_unused]] const auto ticked = engine.Tick(dispatcher);
    }
    while (engine.Stage() != uring::DrainStage::kClosed) {
      if (engine.ShutdownWith(dispatcher, std::chrono::milliseconds(100))) {
        break;
      }
    }
    if (sink.Quiescent()) {
      if (auto flushed = sink.ShutdownFlushSync(); !flushed && worker_error_ == ErrorCode::kSuccess) {
        worker_error_ = flushed.error();
      }
    } else if (worker_error_ == ErrorCode::kSuccess) {
      worker_error_ = ErrorCode::kConsumerBusy;
    }
  }

  void RunFallbackOnStack(core::ConsumerCore& core, RotationType& rotation) noexcept {
    FutexFallbackSink<RotationType> fallback{core, rotation, config_.sink};
    backend_.store(WorkerBackend::kFutexFallback, std::memory_order_release);
    startup_state_.store(1U, std::memory_order_release);
    if (auto res = fallback.Run(stop_requested_); !res) {
      worker_error_ = res.error();
    }
  }

  pthread_t thread_{};
  std::optional<core::LogConsumer> consumer_{};
  std::array<char, RotationType::kPathCap> base_path_buf_{};
  std::size_t base_path_len_{0U};
  WorkerConfig config_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint32_t> startup_state_{0U};
  std::atomic<WorkerBackend> backend_{WorkerBackend::kNone};
  std::atomic<std::size_t> worker_stack_size_{0U};
  ErrorCode startup_error_{ErrorCode::kSuccess};
  ErrorCode worker_error_{ErrorCode::kSuccess};
};

namespace detail {
union GlobalWorkerStorage {
  char dummy{};
  UringConsumerThread<> value;
  constexpr GlobalWorkerStorage() noexcept : dummy{} {}
  ~GlobalWorkerStorage() noexcept {}
};
inline constinit GlobalWorkerStorage g_global_worker{};
inline constinit std::atomic<bool> g_global_worker_initialized{false};
inline constinit std::atomic<std::uint64_t> g_global_worker_generation{0U};

[[nodiscard]] inline UringConsumerThread<>& GlobalWorker() noexcept {
  bool expected = false;
  if (g_global_worker_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
    std::construct_at(&g_global_worker.value);
  }
  return g_global_worker.value;
}
}  // namespace detail

/**
 * @brief Move-only RAII guard owning one background consumer worker generation.
 *
 * Dropping `LoggerGuard` stops and flushes its active worker generation; calling `Shutdown()` returns
 * any flush or rotation error explicitly. A stale guard cannot stop a newer generation.
 */
class [[nodiscard]] LoggerGuard final {
 public:
  constexpr LoggerGuard() noexcept = default;
  LoggerGuard(const LoggerGuard&) = delete;
  LoggerGuard& operator=(const LoggerGuard&) = delete;
  LoggerGuard(LoggerGuard&& other) noexcept : generation_(std::exchange(other.generation_, 0U)) {}
  LoggerGuard& operator=(LoggerGuard&& other) noexcept {
    if (this != &other) {
      [[maybe_unused]] const auto status = Shutdown();
      generation_ = std::exchange(other.generation_, 0U);
    }
    return *this;
  }
  ~LoggerGuard() noexcept { [[maybe_unused]] const auto status = Shutdown(); }

  /** @brief Stop this generation's background worker and flush all pending log records. */
  Result<void> Shutdown() noexcept {
    const std::uint64_t gen = std::exchange(generation_, 0U);
    if (gen == 0U) return {};
    if (detail::g_global_worker_generation.load(std::memory_order_acquire) != gen) return {};
    auto& worker = detail::GlobalWorker();
    if (!worker.Running()) return {};
    return worker.Stop();
  }

  [[nodiscard]] bool Active() const noexcept {
    return generation_ != 0U && detail::g_global_worker_generation.load(std::memory_order_acquire) == generation_ &&
           detail::GlobalWorker().Running();
  }

  [[nodiscard]] WorkerBackend Backend() const noexcept {
    return Active() ? detail::GlobalWorker().Backend() : WorkerBackend::kNone;
  }

 private:
  friend class Builder;
  explicit constexpr LoggerGuard(std::uint64_t generation) noexcept : generation_(generation) {}
  std::uint64_t generation_{0U};
};

/**
 * @brief Fluent builder configuring and starting the `zerolog` background worker or Sans-IO consumer.
 */
class Builder final {
 public:
  constexpr Builder() noexcept = default;

  constexpr Builder& Level(abi::Level level) noexcept {
    level_ = level;
    return *this;
  }

  constexpr Builder& FlushOn(abi::Level level) noexcept {
    flush_level_ = level;
    return *this;
  }

  constexpr Builder& LogFile(std::string_view path) noexcept {
    base_path_ = path;
    return *this;
  }

  constexpr Builder& OutputFd(std::int32_t fd) noexcept {
    output_fd_ = fd;
    return *this;
  }

  constexpr Builder& Rotation(RotationPolicy policy) noexcept {
    rotation_ = policy;
    has_rotation_ = true;
    return *this;
  }

  constexpr Builder& QueueFullPolicy(abi::QueueFullPolicy policy) noexcept {
    queue_full_policy_ = policy;
    return *this;
  }

  constexpr Builder& QueueFullCallback(core::QueueFullCallback callback) noexcept {
    queue_full_callback_ = callback;
    return *this;
  }

  constexpr Builder& DrainBudget(abi::DrainBudget budget) noexcept {
    sink_.drain_budget = budget;
    return *this;
  }

  constexpr Builder& IdleTimeoutNs(std::uint64_t ns) noexcept {
    sink_.idle_timeout_ns = ns;
    return *this;
  }

  template <class Rep, class Period>
  constexpr Builder& IdleTimeout(std::chrono::duration<Rep, Period> timeout) noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    sink_.idle_timeout_ns = ns > 0 ? static_cast<std::uint64_t>(ns) : 1ULL;
    return *this;
  }

  constexpr Builder& ManageDeepPark(std::uint8_t empty_ticks) noexcept {
    sink_.manage_deep_park = empty_ticks;
    return *this;
  }

  constexpr Builder& Sink(SinkConfig config) noexcept {
    sink_ = config;
    return *this;
  }

  constexpr Builder& RingEntries(std::uint32_t entries) noexcept {
    ring_entries_ = entries;
    return *this;
  }

  constexpr Builder& Prewarm(bool enable = true) noexcept {
    prewarm_ = enable;
    return *this;
  }

  /** @brief Apply configuration and start the Mode B background worker thread, returning an RAII `LoggerGuard`. */
  [[nodiscard]] Result<LoggerGuard> Start() const noexcept {
    if (!base_path_.empty() && output_fd_ != 2) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (base_path_.empty() && has_rotation_) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (prewarm_) {
      if (auto warmed = core::PrewarmCurrentThread(); !warmed) {
        return std::unexpected(warmed.error());
      }
    }
    core::SetLevel(level_);
    core::FlushOn(flush_level_);
    core::SetQueueFullPolicy(queue_full_policy_);
    core::SetQueueFullCallback(queue_full_callback_);

    const RotationPolicy policy = has_rotation_ ? rotation_ : RotationPolicy{.max_bytes = ~0ULL, .max_files = 1U};
    auto& worker = detail::GlobalWorker();
    auto started = worker.Start(WorkerConfig{
        .base_path = base_path_,
        .rotation = policy,
        .sink = sink_,
        .ring_entries = ring_entries_,
        .fault_uring = std::nullopt,
        .output_fd = base_path_.empty() ? output_fd_ : -1,
    });
    if (!started) {
      return std::unexpected(started.error());
    }
    const std::uint64_t gen = detail::g_global_worker_generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    return LoggerGuard{gen};
  }

  /** @brief Apply configuration without spawning a worker thread and acquire an exclusive `LogConsumer` handle. */
  [[nodiscard]] Result<core::LogConsumer> BuildConsumer() const noexcept {
    if (!base_path_.empty()) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (prewarm_) {
      if (auto warmed = core::PrewarmCurrentThread(); !warmed) {
        return std::unexpected(warmed.error());
      }
    }
    core::SetLevel(level_);
    core::FlushOn(flush_level_);
    core::SetQueueFullPolicy(queue_full_policy_);
    core::SetQueueFullCallback(queue_full_callback_);

    auto consumer = core::AcquireConsumer();
    if (!consumer) return std::unexpected(consumer.error());
    if (const auto status = consumer->SetOutputFd(output_fd_); status != ErrorCode::kSuccess) {
      return std::unexpected(status);
    }
    return consumer;
  }

 private:
  abi::Level level_{abi::Level::kInfo};
  abi::Level flush_level_{abi::Level::kError};
  std::string_view base_path_{};
  std::int32_t output_fd_{2};
  RotationPolicy rotation_{};
  bool has_rotation_{false};
  abi::QueueFullPolicy queue_full_policy_{abi::QueueFullPolicy::kDrop};
  core::QueueFullCallback queue_full_callback_{nullptr};
  SinkConfig sink_{};
  std::uint32_t ring_entries_{64U};
  bool prewarm_{true};
};

}  // namespace zerolog::sink

namespace zerolog {
using sink::Builder;
using sink::LoggerGuard;
using sink::UringConsumerThread;
using sink::WorkerBackend;
using sink::WorkerConfig;
}  // namespace zerolog
