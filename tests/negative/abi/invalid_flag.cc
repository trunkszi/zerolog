// expected-error: static assertion|Flag bit
#include <zerolog/abi/contract.hpp>
struct Bad {
  using Word = std::uint64_t;
  bool flag[[= uarch::bits::Flag{31}]];
};
static_assert(zerolog::abi::HotFlagsValid<Bad>(), "hot Flag bit must be below 31");
