// expected-error: CodecErrorRawPointer
#include <zerolog/encode/codec.hpp>

struct RawPointer {
  int* field;
};

static_assert(sizeof(zerolog::encode::Codec<RawPointer>) > 0);
