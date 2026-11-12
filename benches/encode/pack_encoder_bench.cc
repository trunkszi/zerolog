#include <cstddef>
#include <cstdint>
#include <zerolog/encode/pack_encoder.hpp>

int main() {
  std::byte dest[16]{};
  std::uint64_t a = 1;
  std::uint64_t b = 2;
  std::uint64_t sink = 0;
  for (int i = 0; i < 1000; ++i) {
    a += 1;
    if (zerolog::encode::EncodeRecord(dest, sizeof(dest), a, b) != zerolog::ErrorCode::kSuccess) return 1;
    sink += static_cast<std::uint64_t>(dest[0]);
  }
  volatile std::uint64_t published = sink;
  return published == 0 ? 1 : 0;
}
