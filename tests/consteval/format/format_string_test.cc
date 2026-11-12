#include <cstdint>
#include <type_traits>
#include <zerolog/format/consteval_parser.hpp>

enum class Color : unsigned { kRed = 1, kBlue = 2, kCrimson = 1, kNone = 0 };

constexpr auto kPlain = zerolog::format::FormatString<zerolog::abi::Level::kInfo, int>{"n={}"};
static_assert(kPlain.info->arg_count == 1);
static_assert(kPlain.info->segment_count == 2);
static_assert(kPlain.info->level == zerolog::abi::Level::kInfo);
static_assert(kPlain.info->segments[0].kind == zerolog::abi::SegmentKind::kLiteral);
static_assert(kPlain.info->segments[0].text_len == 2);
static_assert(kPlain.info->segments[1].kind == zerolog::abi::SegmentKind::kArg);
static_assert(kPlain.info->segments[1].presentation == zerolog::abi::FormatPresentation::kDefault);

constexpr auto kSpecs = zerolog::format::FormatString<zerolog::abi::Level::kDebug, unsigned, double>{"{:x}:{:.2f}"};
static_assert(kSpecs.info->segment_count == 3);
static_assert(kSpecs.info->segments[0].presentation == zerolog::abi::FormatPresentation::kHexLower);
static_assert(kSpecs.info->segments[1].kind == zerolog::abi::SegmentKind::kLiteral);
static_assert(kSpecs.info->segments[1].text_len == 1);
static_assert(kSpecs.info->segments[2].presentation == zerolog::abi::FormatPresentation::kFixedFloat);
static_assert(kSpecs.info->segments[2].precision == 2);

constexpr auto kUpper = zerolog::format::FormatString<zerolog::abi::Level::kWarn, int>{"{:X}"};
static_assert(kUpper.info->segments[0].presentation == zerolog::abi::FormatPresentation::kHexUpper);
constexpr auto kBinary = zerolog::format::FormatString<zerolog::abi::Level::kError, int>{"{:b}"};
static_assert(kBinary.info->segments[0].presentation == zerolog::abi::FormatPresentation::kBinary);

constexpr auto kEscaped = zerolog::format::FormatString<zerolog::abi::Level::kInfo>{"{{ok}}"};
static_assert(kEscaped.info->segment_count == 1);
static_assert(kEscaped.info->segments[0].text_len == 4);
static_assert(kEscaped.info->arg_count == 0);

constexpr auto kOtherLevel = zerolog::format::FormatString<zerolog::abi::Level::kError, int>{"x={}"};
static_assert(kPlain.info->decode == kOtherLevel.info->decode);

constexpr auto kEnum = zerolog::format::FormatString<zerolog::abi::Level::kTrace, Color>{"{}"};
static_assert(kEnum.info->arg_count == 1);
static_assert(kEnum.info->level == zerolog::abi::Level::kTrace);

template <class... A>
constexpr auto LogCall(
    zerolog::format::FormatString<zerolog::abi::Level::kInfo, std::type_identity_t<std::remove_cvref_t<A>>...> fmt,
    const A &...) {
  return fmt;
}

constexpr auto kDeduced = LogCall("x={} name={}", 42, "bob");
static_assert(kDeduced.info->arg_count == 2);
static_assert(kDeduced.info->segment_count == 4);
static_assert(kDeduced.info->segment_count != zerolog::abi::kMaxFormatSegments);
static_assert(kPlain.info->segment_count != zerolog::abi::kMaxFormatSegments);
static_assert(kDeduced.info->segment_count != kPlain.info->segment_count);
static_assert(kDeduced.info->segments[0].kind == zerolog::abi::SegmentKind::kLiteral);
static_assert(kDeduced.info->segments[0].text_len == 2);
static_assert(kDeduced.info->segments[2].kind == zerolog::abi::SegmentKind::kLiteral);
static_assert(kDeduced.info->segments[2].text_len == 6);

constexpr auto kOneArg = LogCall("{}", 1);
static_assert(kOneArg.info->segment_count == 1);
static_assert(kOneArg.info->segment_count != kDeduced.info->segment_count);
static_assert(kOneArg.info->segment_count != zerolog::abi::kMaxFormatSegments);

int main() { return 0; }
