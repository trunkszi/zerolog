#include <cstdio>
#include <zerolog/core/producer.hpp>
int main() {
  using namespace zerolog;
  core::SetLevel(abi::Level::kTrace);
  core::FlushOn(abi::Level::kOff);
  auto buffer = core::PrewarmCurrentThread();
  if (!buffer) return static_cast<int>(buffer.error());
  std::uint64_t cycles = 0;
  constexpr std::uint32_t batch = 4096;
  constexpr std::uint32_t rounds = 256;
  for (std::uint32_t round = 0; round < rounds; ++round) {
    const auto start = sys::ReadTscSerialized();
    for (std::uint32_t n = 0; n < batch; ++n)
      if (const auto error = TryInfo("bench {} {}", std::uint64_t{n}, std::uint64_t{round}); error != ErrorCode::kSuccess)
        return static_cast<int>(error);
    cycles += sys::ReadTscSerialized() - start;
    for (;;) {
      const auto view = (*buffer)->queue.Front();
      if (!view.Valid()) break;
      (*buffer)->queue.Pop(view);
    }
  }
  std::printf("producer cycles/record %.2f\n", static_cast<double>(cycles) / (batch * rounds));
}
