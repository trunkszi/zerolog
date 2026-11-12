#include <atomic>
#include <thread>
#include <zerolog/sys/tsc_clock.hpp>

#include "support/check.hpp"
int main() {
  using namespace zerolog;
  auto clock = sys::TscClock::Calibrate();
  if (!clock) {
    CHECK(clock.error() == ErrorCode::kInvalidArgument);
    return 0;
  }
  std::uint64_t sampled_tsc{};
  std::int64_t sampled_ns{};
  CHECK(sys::TscClock::SyncTime(sampled_tsc, sampled_ns));
  CHECK(sampled_ns > 0 && sampled_tsc > 0);
  CHECK(clock->Reanchor(sampled_tsc, sampled_ns));
  CHECK(clock->ToWallNanoseconds(sampled_tsc) == static_cast<std::uint64_t>(sampled_ns));
  CHECK(!clock->Reanchor(sampled_tsc, -1));
  CHECK(clock->Calibrate(0));
  CHECK(clock->ToWallNanoseconds(sampled_tsc) == static_cast<std::uint64_t>(sampled_ns));
  // A wall-clock step changes the anchor, rather than silently changing the tick rate.
  CHECK(clock->Reanchor(sampled_tsc, sampled_ns - 5'000'000));
  CHECK(clock->ToWallNanoseconds(sampled_tsc) == static_cast<std::uint64_t>(sampled_ns - 5'000'000));
  const auto step_rate =
      clock->ToWallNanoseconds(sampled_tsc + 1'000'000'000) - static_cast<std::uint64_t>(sampled_ns - 5'000'000);
  CHECK(clock->Reanchor(sampled_tsc, sampled_ns));
  CHECK(clock->ToWallNanoseconds(sampled_tsc + 1'000'000'000) - static_cast<std::uint64_t>(sampled_ns) == step_rate);
  CHECK(clock->Reanchor(sampled_tsc + 1'000'000'000, sampled_ns + static_cast<std::int64_t>(step_rate * 3 / 4)));
  const auto refined = clock->ToWallNanoseconds(sampled_tsc + 2'000'000'000) -
                       static_cast<std::uint64_t>(sampled_ns + static_cast<std::int64_t>(step_rate * 3 / 4));
  CHECK(refined > step_rate * 93 / 100 && refined < step_rate * 95 / 100);
  CHECK(clock->Reanchor());
  CHECK(clock->Calibrate(std::numeric_limits<std::uint64_t>::max()));
  const auto fixed_tsc = sys::ReadTsc();
  constexpr std::int64_t first = 1'000'000'000'000;
  constexpr std::int64_t second = 2'000'000'000'000;
  CHECK(clock->Reanchor(fixed_tsc, first));
  uarch::Isolated<std::atomic<bool>> stop{};
  std::thread reader([&] {
    do {
      const auto ns = clock->ToWallNanoseconds(fixed_tsc);
      CHECK(ns == static_cast<std::uint64_t>(first) || ns == static_cast<std::uint64_t>(second));
    } while (!stop.value.load(std::memory_order_acquire));
  });
  for (int i = 0; i < 10000; ++i) CHECK(clock->Reanchor(fixed_tsc, i % 2 == 0 ? second : first));
  stop.value.store(true, std::memory_order_release);
  reader.join();
  CHECK(clock->ToWallNanoseconds(0) <= static_cast<std::uint64_t>(first));
}
