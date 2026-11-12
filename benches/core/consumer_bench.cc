#include <cstdio>
#include <zerolog/core/consumer_core.hpp>
#include <zerolog/core/producer.hpp>
int main() {
  using namespace zerolog;
  core::SetLevel(abi::Level::kTrace);
  core::FlushOn(abi::Level::kOff);
  if (!core::PrewarmCurrentThread()) return 1;
  auto consumer = core::ConsumerCore::Create();
  if (!consumer) return static_cast<int>(consumer.error());
  std::uint64_t cycles = 0;
  constexpr std::uint32_t batch = 4096;
  constexpr std::uint32_t rounds = 64;
  for (std::uint32_t round = 0; round < rounds; ++round) {
    for (std::uint32_t n = 0; n < batch; ++n)
      if (const auto error = TryInfo("bench {} {}", std::uint64_t{n}, std::uint64_t{round}); error != ErrorCode::kSuccess)
        return static_cast<int>(error);
    const auto start = sys::ReadTscSerialized();
    auto drained = consumer->DrainBatch(start, {batch, format::kMaxPendingBytes});
    cycles += sys::ReadTscSerialized() - start;
    if (!drained) return static_cast<int>(drained.error());
    if (drained->records != batch) return 1;
    if (auto slab = consumer->Output().Freeze()) {
      if (!consumer->Output().CompleteFrozen(*slab, slab->length)) return 1;
    }
  }
  std::printf("consumer format cycles/record %.2f\n", static_cast<double>(cycles) / (batch * rounds));
}
