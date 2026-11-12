// expected-error: static assertion|field
#include <zerolog/abi/contract.hpp>
struct Bad {
  using Word = std::uint32_t;
  unsigned x[[= uarch::bits::Field{31, 2}]];
};
uarch::BitWord<Bad> word;
