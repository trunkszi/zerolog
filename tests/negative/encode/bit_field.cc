// expected-error: CodecErrorBitField
#include <zerolog/encode/codec.hpp>

struct BitField {
  unsigned value : 3;
};

static_assert(sizeof(zerolog::encode::Codec<BitField>) > 0);
