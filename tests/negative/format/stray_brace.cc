// expected-error: constexpr
#include <zerolog/format/consteval_parser.hpp>

constexpr auto kBad = zerolog::format::FormatString<zerolog::abi::Level::kInfo>{"}"};
