#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <zerolog/core/spsc_var_queue.hpp>

namespace {

using namespace zerolog;
using namespace zerolog::core;

[[gnu::noinline]] bool BenchRoundTrip(SpscVarQueue& q, std::uint64_t seq, std::uint64_t& checksum) noexcept {
  constexpr std::uint32_t blocks = 6U;
  std::byte* rec = q.TryReserve(blocks);
  if (rec == nullptr && (rec = q.ReserveSlow(blocks)) == nullptr) {
    return false;
  }
  std::memcpy(rec + 16, &seq, sizeof(seq));
  std::memcpy(rec + 32, &seq, sizeof(seq));
  const auto header = abi::RecordHeaderWord{}
                          .with<^^abi::RecordHeaderSpec::blocks>(blocks)
                          .with<^^abi::RecordHeaderSpec::level>(abi::Level::kInfo)
                          .with<^^abi::RecordHeaderSpec::payload_bytes>((blocks - 4U) * 8U);
  SpscVarQueue::Publish(rec, header);

  RecordView view = q.Front();
  if (!view.Valid()) {
    return false;
  }
  checksum += view.Prefix()->tsc;
  q.Pop(view);
  return true;
}

}  // namespace

int main() {
  using Clock = std::chrono::steady_clock;
  auto q_res = SpscVarQueue::Create();
  if (!q_res) {
    return 1;
  }
  auto& q = *q_res;
  std::array<double, 8192> samples{};
  std::uint64_t checksum = 0U;

  for (std::size_t i = 0; i < samples.size(); ++i) {
    const auto start = Clock::now();
    if (!BenchRoundTrip(q, i + 1U, checksum)) {
      return 2;
    }
    samples[i] = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  }

  std::sort(samples.begin(), samples.end());
  std::printf("spsc roundtrip ns p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f checksum=%llu\n", samples[4096], samples[7372],
              samples[8110], samples[8183], static_cast<unsigned long long>(checksum));
  return checksum == 33558528ULL ? 0 : 3;
}
