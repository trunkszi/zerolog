// expected-error: CodecErrorReference
#include <zerolog/encode/codec.hpp>

struct Reference {
  int& field;
};

static_assert(sizeof(zerolog::encode::Codec<Reference>) > 0);
