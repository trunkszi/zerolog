/** @file @brief Microbenchmarks for sink FSM transitions and `ProactorLogSink::OnCompletions`. */

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <zerolog/abi/contract.hpp>
#include <zerolog/sink/wake_timer_fsm.hpp>
#include <zerolog/sink/write_rotate_fsm.hpp>

int main() {
  using namespace zerolog::sink;

  constexpr std::uint64_t iterations = 1'000'000ULL;
  zerolog::abi::SinkFlagsWord flags =
      zerolog::abi::SinkFlagsWord{}
          .with<^^zerolog::abi::SinkFlagsSpec::lifecycle>(zerolog::abi::LifecycleState::kRunning)
          .with<^^zerolog::abi::SinkFlagsSpec::io>(zerolog::abi::WriteRotateState::kReady);
  WriteRotateFsm writer{};
  [[maybe_unused]] const auto bound = writer.Bind(3U);

  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    [[maybe_unused]] const auto w1 = WakeFsm::TryArm(flags);
    [[maybe_unused]] const auto w2 = WakeFsm::OnWakeCqe(flags, 0);
    [[maybe_unused]] const auto t1 = TimerFsm::TryArm(flags);
    [[maybe_unused]] const auto t2 = TimerFsm::OnTimerCqe(flags, -ETIME);
    [[maybe_unused]] const auto io1 = writer.BeginWrite(flags, 4096U);
    [[maybe_unused]] const auto io2 = writer.OnWriteCqe(flags, 4096, 4096U);
    auto raw = flags.raw();
    asm volatile("" : "+r"(raw));
  }
  const auto end = std::chrono::steady_clock::now();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  std::printf("zerolog_sink_bench: %llu iterations in %lld ns (%.2f ns/iter)\n", static_cast<unsigned long long>(iterations),
              static_cast<long long>(ns), static_cast<double>(ns) / static_cast<double>(iterations));
  return 0;
}
