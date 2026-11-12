/** @file @brief Invariant-TSC timestamps with periodically reanchored Q32 wall-time conversion. */
#pragma once
#include <time.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <uarch/detect/detect.hpp>
#include <uarch/uarch.hpp>
#include <zerolog/error/zerolog_error.hpp>
namespace zerolog::sys {
/** @brief Unserialized producer timestamp; it establishes no memory-ordering boundary. */
[[nodiscard, gnu::always_inline]] inline std::uint64_t ReadTsc() noexcept { return __builtin_ia32_rdtsc(); }
/** @brief Serialized sample for callers measuring instruction intervals. */
[[nodiscard, gnu::always_inline]] inline std::uint64_t ReadTscSerialized() noexcept {
  std::uint32_t low, high;
  asm volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
  return (std::uint64_t{high} << 32) | low;
}
/** @brief One calibration writer and concurrent conversion readers. Moves require quiescence. */
class TscClock final {
  struct Snapshot {
    std::uint64_t base_tsc;
    std::int64_t base_ns;
    std::uint64_t mul_q32;
    std::uint64_t next_calibrate_tsc;
    std::uint32_t shift;
  };
  class Parameters {
    friend class TscClock;
    std::atomic<std::uint32_t> param_seq_{0};          /**< Even versions identify published parameter sets. */
    std::atomic<std::uint64_t> base_tsc_{0};           /**< TSC at the wall-time anchor. */
    std::atomic<std::int64_t> base_ns_{0};             /**< CLOCK_REALTIME nanoseconds at the anchor. */
    std::atomic<std::uint64_t> mul_q32_{0};            /**< Nanoseconds per tick in Q32 format. */
    std::atomic<std::uint32_t> shift_{32};             /**< Fixed-point fractional width. */
    std::atomic<std::uint64_t> next_calibrate_tsc_{0}; /**< Earliest scheduled refresh. */
  };
  uarch::Isolated<Parameters> params_; /**< Isolates the calibration publication from unrelated writers. */
  __extension__ using Wide = unsigned __int128;
  static constexpr std::uint64_t kIntervalNs = 1'000'000'000;

  explicit TscClock(Snapshot values) noexcept { Publish(values); }
  [[nodiscard, gnu::always_inline]] Snapshot ReadParameters() const noexcept {
    const auto& p = params_.value;
    for (;;) {
      const auto before = p.param_seq_.load(std::memory_order_acquire);
      if (before & 1U) continue;
      std::atomic_signal_fence(std::memory_order_acq_rel);
      Snapshot result{p.base_tsc_.load(std::memory_order_relaxed), p.base_ns_.load(std::memory_order_relaxed),
                      p.mul_q32_.load(std::memory_order_relaxed), p.next_calibrate_tsc_.load(std::memory_order_relaxed),
                      p.shift_.load(std::memory_order_relaxed)};
      // Atomic payloads avoid the ordinary-field data race of a conventional seqlock.
      std::atomic_thread_fence(std::memory_order_acquire);
      std::atomic_signal_fence(std::memory_order_acq_rel);
      if (before == p.param_seq_.load(std::memory_order_acquire)) return result;
    }
  }
  void Publish(Snapshot values) noexcept {
    auto& p = params_.value;
    const auto seq = p.param_seq_.load(std::memory_order_relaxed);
    p.param_seq_.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::atomic_signal_fence(std::memory_order_acq_rel);
    p.base_tsc_.store(values.base_tsc, std::memory_order_relaxed);
    p.base_ns_.store(values.base_ns, std::memory_order_relaxed);
    p.mul_q32_.store(values.mul_q32, std::memory_order_relaxed);
    p.shift_.store(values.shift, std::memory_order_relaxed);
    p.next_calibrate_tsc_.store(values.next_calibrate_tsc, std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_acq_rel);
    p.param_seq_.store(seq + 2, std::memory_order_release);
  }
  [[nodiscard]] static std::uint64_t Deadline(std::uint64_t tsc, std::uint64_t mul) noexcept {
    const auto ticks = (static_cast<Wide>(kIntervalNs) << 32) / mul;
    return uarch::arithmetic::saturating_add(tsc, static_cast<std::uint64_t>(ticks));
  }

 public:
  TscClock(const TscClock&) = delete ("calibration has one writer");
  TscClock& operator=(const TscClock&) = delete ("calibration has one writer");
  /** @brief Transfer a quiescent clock into its final owner. */
  TscClock(TscClock&& other) noexcept : TscClock(other.ReadParameters()) {}
  /** @brief Replace a quiescent clock; no reader may overlap this assignment. */
  TscClock& operator=(TscClock&& other) noexcept {
    if (this != &other) Publish(other.ReadParameters());
    return *this;
  }
  /** @brief Pair realtime and TSC using the shortest of three sampling brackets. */
  [[nodiscard]] static Result<void> SyncTime(std::uint64_t& tsc_out, std::int64_t& ns_out) noexcept {
    std::array<std::uint64_t, 4> tsc{};
    std::array<std::int64_t, 4> ns{};
    tsc[0] = ReadTsc();
    for (std::size_t i = 1; i < tsc.size(); ++i) {
      timespec wall{};
      if (::clock_gettime(CLOCK_REALTIME, &wall) != 0) return std::unexpected(ErrorCode::kInvalidArgument);
      tsc[i] = ReadTsc();
      if (wall.tv_sec < 0 || wall.tv_sec > (std::numeric_limits<std::int64_t>::max() - wall.tv_nsec) / 1'000'000'000)
        return std::unexpected(ErrorCode::kInvalidArgument);
      ns[i] = static_cast<std::int64_t>(wall.tv_sec) * 1'000'000'000 + wall.tv_nsec;
      if (tsc[i] < tsc[i - 1]) return std::unexpected(ErrorCode::kInvalidArgument);
    }
    std::size_t best = 1;
    for (std::size_t i = 2; i < tsc.size(); ++i)
      if (tsc[i] - tsc[i - 1] < tsc[best] - tsc[best - 1]) best = i;
    tsc_out = tsc[best - 1] + ((tsc[best] - tsc[best - 1]) >> 1);
    ns_out = ns[best];
    return {};
  }
  /** @brief Establish the initial rate; missing hardware TSC is an invalid clock configuration. */
  [[nodiscard, gnu::cold, gnu::noinline]] static Result<TscClock> Calibrate() noexcept {
    const auto report = uarch::detect::HostHardwareAudit::probe();
    const auto cpuid1 = uarch::detect::HardwareCpuid{}.cpuid(1, 0);
    if (report.invariant_tsc.is_unsupported() || (cpuid1.edx & (1U << 4)) == 0)
      return std::unexpected(ErrorCode::kInvalidArgument);
    std::uint64_t first{}, last{};
    std::int64_t start{}, finish{};
    if (!SyncTime(first, start)) return std::unexpected(ErrorCode::kInvalidArgument);
    timespec delay{0, 20'000'000};
    while (::nanosleep(&delay, &delay) != 0)
      if (errno != EINTR) return std::unexpected(ErrorCode::kInvalidArgument);
    if (!SyncTime(last, finish) || last <= first || finish <= start) return std::unexpected(ErrorCode::kInvalidArgument);
    const auto rate = (static_cast<Wide>(finish - start) << 32) / (last - first);
    if (rate == 0 || rate > std::numeric_limits<std::uint64_t>::max()) return std::unexpected(ErrorCode::kInvalidArgument);
    const auto mul = static_cast<std::uint64_t>(rate);
    return TscClock{Snapshot{last, finish, mul, Deadline(last, mul), 32}};
  }
  /** @brief Reanchor to an external paired sample. Wall-clock steps retain the previous rate. */
  [[nodiscard, gnu::cold]] Result<void> Reanchor(std::uint64_t tsc, std::int64_t ns) noexcept {
    if (ns < 0) return std::unexpected(ErrorCode::kInvalidArgument);
    auto values = ReadParameters();
    if (tsc > values.base_tsc && ns > values.base_ns) {
      const auto measured = (static_cast<Wide>(ns - values.base_ns) << 32) / (tsc - values.base_tsc);
      // Large wall-clock steps must not turn into a permanent change in tick frequency.
      if (measured > 0 && measured >= values.mul_q32 / 2 && measured <= static_cast<Wide>(values.mul_q32) * 2) {
        const auto refined = (static_cast<Wide>(values.mul_q32) * 3 + measured) / 4;
        if (refined <= std::numeric_limits<std::uint64_t>::max()) values.mul_q32 = static_cast<std::uint64_t>(refined);
      }
    }
    values.base_tsc = tsc;
    values.base_ns = ns;
    values.next_calibrate_tsc = Deadline(tsc, values.mul_q32);
    Publish(values);
    return {};
  }
  /** @brief Force a fresh realtime sample and rate refinement from the calibration writer. */
  [[nodiscard, gnu::cold]] Result<void> Reanchor() noexcept {
    std::uint64_t tsc{};
    std::int64_t ns{};
    if (auto result = SyncTime(tsc, ns); !result) return result;
    return Reanchor(tsc, ns);
  }
  /** @brief Refresh at most once per interval; the consumer supplies its current timestamp. */
  [[nodiscard, gnu::cold]] Result<void> Calibrate(std::uint64_t now_tsc) noexcept {
    if (now_tsc < ReadParameters().next_calibrate_tsc) return {};
    return Reanchor();
  }
  /** @brief Convert even historical timestamps; results saturate at the unsigned wall-time limits. */
  [[nodiscard, gnu::always_inline]] std::uint64_t ToWallNanoseconds(std::uint64_t tsc) const noexcept {
    const auto values = ReadParameters();
    [[assume(values.shift == 32)]];
    const auto delta = tsc >= values.base_tsc ? tsc - values.base_tsc : values.base_tsc - tsc;
    const auto product = (static_cast<Wide>(delta) * values.mul_q32) >> 32;
    const auto scaled = product > std::numeric_limits<std::uint64_t>::max() ? std::numeric_limits<std::uint64_t>::max()
                                                                            : static_cast<std::uint64_t>(product);
    const auto base = static_cast<std::uint64_t>(values.base_ns);
    return tsc >= values.base_tsc ? uarch::arithmetic::saturating_add(base, scaled)
                                  : uarch::arithmetic::saturating_sub(base, scaled);
  }
};
}  // namespace zerolog::sys
