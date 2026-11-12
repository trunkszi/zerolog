// expected-error: CodecErrorPrivateMember
#include <zerolog/encode/codec.hpp>

struct PrivateMember {
  int open;

 private:
  int hidden;
};

static_assert(sizeof(zerolog::encode::Codec<PrivateMember>) > 0);
