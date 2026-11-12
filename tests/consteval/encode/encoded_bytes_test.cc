#include <cstddef>
#include <cstdint>
#include <string_view>
#include <zerolog/encode/pack_encoder.hpp>

struct Pair {
  std::uint32_t a;
  std::uint32_t b;
};

struct Msg {
  std::string_view name;
  int id;
};

static_assert(zerolog::encode::EncodedBytes() == 0);
static_assert(zerolog::encode::EncodedBytes(std::uint64_t{1}, std::uint64_t{2}) == 16);
static_assert(zerolog::encode::EncodedBytes(Pair{1, 2}) == 8);
static_assert(!zerolog::encode::Codec<Msg>::kFixed);
static_assert(zerolog::encode::EncodedBytes(std::string_view{"ab"}) == 6);

int main() { return 0; }
