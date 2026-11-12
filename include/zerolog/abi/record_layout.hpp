/** @file @brief Wire record prefixes, immutable format descriptors and bounded output appends. */
#pragma once
#include <cstddef>
#include <zerolog/abi/contract.hpp>
#include <zerolog/error/zerolog_error.hpp>

namespace zerolog::abi {
using Block = uarch::PowerOfTwo<3>;
inline constexpr std::uint32_t kLogRingBlocks = 18;
using RingBlocks = uarch::PowerOfTwo<kLogRingBlocks>;
inline constexpr std::uint32_t kRingBytes = RingBlocks::value * Block::value;
inline constexpr std::uint32_t kWrapSentinel = 1;
inline constexpr std::uint32_t kMaxRecordBlocks = RingBlocks::value / 8;
inline constexpr std::size_t kMaxFormatSegments = 128;
inline constexpr std::size_t kMaxFormatArgs = 64;

struct RecordHeaderSpec {
  using Word = std::uint64_t;
  std::uint32_t blocks[[= uarch::bits::Field{0, 20}]]; /**< Record extent in eight-byte blocks. */
  Level level[[= uarch::bits::Field{20, 3}]];          /**< Record severity. */
  bool truncated[[= uarch::bits::Flag{23}]];
  std::uint32_t payload_bytes[[= uarch::bits::Field{32, 32}]]; /**< Encoded payload length excluding the prefix. */
};
static_assert(HotFlagsValid<RecordHeaderSpec>(), "hot Flag bit must be below 31");
using RecordHeaderWord = uarch::BitWord<RecordHeaderSpec>;

struct StaticRecordInfo;

struct RecordPrefix {
  std::uint64_t header{};         /**< Release-published RecordHeaderWord bits. */
  const StaticRecordInfo* info{}; /**< Process-lifetime immutable format descriptor. */
  std::uint64_t tsc{};            /**< Unserialized producer timestamp. */
  std::uint64_t span_id{};        /**< Producer span identifier. */
};
static_assert(sizeof(RecordPrefix) == 32 && uarch::NoPadding<RecordPrefix>);

struct DecodeCursor {
  const std::byte* data{};   /**< Next encoded byte. */
  std::uint32_t remaining{}; /**< Readable bytes left in the cursor. */
  std::uint32_t reserved{};  /**< Must remain zero. */
};
static_assert(sizeof(DecodeCursor) == 16 && uarch::NoPadding<DecodeCursor> && uarch::GprPair<DecodeCursor>);

/** @brief A bounded view used for transactional per-record formatting. */
class OutputAppender {
 public:
  /** @brief Borrow an output span and length; the initial length must not exceed cap. */
  constexpr OutputAppender(char* base, std::size_t* len, std::size_t cap) noexcept : base_(base), len_(len), cap_(cap) {}
  /** @brief Bytes available under the current total-output quota. */
  [[nodiscard, gnu::always_inline]] constexpr std::size_t Remaining() const noexcept { return cap_ - *len_; }
  /** @brief Writable tail for a caller that has already checked Remaining(). */
  [[nodiscard, gnu::always_inline]] constexpr char* WritablePtr() noexcept { return base_ + *len_; }
  /** @brief Commit at most Remaining() bytes previously written by the caller. */
  [[gnu::always_inline]] constexpr void Advance(std::size_t n) noexcept { *len_ += n; }
  /** @brief Append a non-overlapping byte span; quota failure leaves the view unchanged. */
  [[nodiscard, gnu::always_inline]] constexpr ErrorCode Append(const char* src, std::size_t n) noexcept {
    if (uarch::unlikely(n > Remaining())) return ErrorCode::kWouldBlock;
    __builtin_memcpy(base_ + *len_, src, n);
    *len_ += n;
    return ErrorCode::kSuccess;
  }
  /** @brief Append one byte or report quota exhaustion without mutation. */
  [[nodiscard, gnu::always_inline]] constexpr ErrorCode Push(char c) noexcept {
    if (uarch::unlikely(Remaining() == 0)) return ErrorCode::kWouldBlock;
    base_[(*len_)++] = c;
    return ErrorCode::kSuccess;
  }

 private:
  char* base_;
  std::size_t* len_;
  std::size_t cap_;
};

using DecodeFn = ErrorCode (*)(const StaticRecordInfo* info, DecodeCursor cursor, OutputAppender& out) noexcept;

enum class SegmentKind : std::uint8_t {
  kLiteral = 0,
  kArg = 1,
};

enum class FormatPresentation : std::uint8_t {
  kDefault = 0,
  kHexLower = 1,
  kHexUpper = 2,
  kBinary = 3,
  kFixedFloat = 4,
};

struct FormatSegment {
  const char* text{};                                            /**< Static literal storage for this segment. */
  std::uint32_t text_len{};                                      /**< Literal byte count. */
  std::uint16_t arg_index{};                                     /**< Zero-based decoded argument index. */
  SegmentKind kind{SegmentKind::kLiteral};                       /**< Literal or argument segment. */
  FormatPresentation presentation{FormatPresentation::kDefault}; /**< Formatting presentation selected at compile time. */
  std::uint8_t precision{6};                                     /**< Fixed-float decimal precision. */
  std::uint8_t reserved[7]{};                                    /**< Must remain zero. */
};
static_assert(sizeof(FormatSegment) == 24 && uarch::NoPadding<FormatSegment>);

struct StaticRecordInfo {
  DecodeFn decode{};               /**< Shared decoder for this argument type pack. */
  const FormatSegment* segments{}; /**< Static array of compiled format operations. */
  const char* format{};            /**< Original promoted format string. */
  const char* file{};              /**< Promoted call-site filename. */
  const char* function{};          /**< Promoted call-site function name. */
  std::uint32_t format_len{};      /**< Format string byte count. */
  std::uint32_t file_len{};        /**< Filename byte count. */
  std::uint32_t function_len{};    /**< Function name byte count. */
  std::uint32_t line{};            /**< Call-site source line. */
  std::uint16_t segment_count{};   /**< Number of format operations. */
  std::uint8_t arg_count{};        /**< Number of encoded arguments. */
  Level level{Level::kInfo};       /**< Record severity. */
  std::uint32_t column{};          /**< Call-site source column. */
};
static_assert(sizeof(StaticRecordInfo) == 64 && uarch::NoPadding<StaticRecordInfo>);
}  // namespace zerolog::abi
