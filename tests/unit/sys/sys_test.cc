#include <utility>
#include <zerolog/sys/thp_mmap.hpp>
#include <zerolog/sys/tsc_clock.hpp>

#include "support/check.hpp"
int main() {
  using namespace zerolog;
  auto ring = sys::ThpRingMapping::Create();
  CHECK(ring);
  CHECK(ring->Bytes().size() == 2 * 1024 * 1024 && ring->MappedSize() == 4 * 1024 * 1024);
  CHECK(reinterpret_cast<std::uintptr_t>(ring->Bytes().data()) % (2 * 1024 * 1024) == 0);
  ring->Bytes().front() = std::byte{42};
  ring->Bytes().back() = std::byte{7};
  auto moved = std::move(*ring);
  CHECK(ring->Bytes().empty());
  CHECK(moved.Bytes().front() == std::byte{42} && moved.Bytes().back() == std::byte{7});
  auto slab = sys::SlabMapping::Create();
  CHECK(slab && slab->Bytes().size() == 16 * 1024 * 1024);
  slab->Bytes().front() = std::byte{5};
  slab->Bytes().back() = std::byte{9};
  const auto before = sys::ReadTsc();
  CHECK(sys::ReadTsc() >= before);
  auto clock = sys::TscClock::Calibrate();
  if (clock) {
    timespec wall{};
    CHECK(::clock_gettime(CLOCK_REALTIME, &wall) == 0);
    const auto actual = static_cast<std::uint64_t>(wall.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(wall.tv_nsec);
    const auto predicted = clock->ToWallNanoseconds(sys::ReadTsc());
    CHECK(predicted > actual - 50'000'000 && predicted < actual + 50'000'000);
    CHECK(clock->ToWallNanoseconds(before) <= predicted);
  } else
    CHECK(clock.error() == ErrorCode::kInvalidArgument);
}
