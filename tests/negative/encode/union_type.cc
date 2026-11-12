// expected-error: CodecErrorUnion
#include <zerolog/encode/codec.hpp>

union UnionValue {
  int a;
  float b;
};

static_assert(sizeof(zerolog::encode::Codec<UnionValue>) > 0);
