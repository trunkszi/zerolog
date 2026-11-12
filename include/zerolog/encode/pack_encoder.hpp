/** @file @brief Variadic pack size computation, C-string normalization, payload encoding and record decoding. */
#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <uarch/uarch.hpp>
#include <utility>
#include <zerolog/encode/codec.hpp>

namespace zerolog::encode {

/** @brief Byte size of a normalized argument pack. Fixed packs fold to a
 * constant. */
template <class... Args>
[[nodiscard, gnu::always_inline]] constexpr std::size_t EncodedBytes(const Args &...args) noexcept {
  if constexpr (sizeof...(Args) == 0) {
    return 0;
  } else if constexpr ((Codec<std::remove_cvref_t<Args>>::kFixed && ...)) {
    return (Codec<std::remove_cvref_t<Args>>::kBytes + ... + std::size_t{0});
  } else {
    return (Codec<std::remove_cvref_t<Args>>::Size(args) + ... + std::size_t{0});
  }
}

/** @brief One C-string scan. Other arguments pass through unchanged. */
template <class T>
[[nodiscard, gnu::always_inline]] inline decltype(auto) PrepareArg(const T &arg) noexcept {
  using Raw = std::remove_cvref_t<T>;
  if constexpr (std::is_pointer_v<Raw> && std::is_same_v<std::remove_cv_t<std::remove_pointer_t<Raw>>, char>) {
    const char *text = arg;
    if (text == nullptr) return std::string_view{};
    return std::string_view{text, std::char_traits<char>::length(text)};
  } else if constexpr (std::is_array_v<Raw> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<Raw>>, char>) {
    return Codec<Raw>::BoundedStringView(arg);
  } else {
    return static_cast<const T &>(arg);
  }
}

/** @brief Little-endian payload encoding. One instantiation exists per
 * normalized pack. */
template <class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode EncodeRecord([[maybe_unused]] std::byte *dest, std::size_t cap,
                                                                [[maybe_unused]] const Args &...args) noexcept {
  std::size_t left = cap;
  if constexpr (sizeof...(Args) == 0) {
    return ErrorCode::kSuccess;
  } else if constexpr ((Codec<std::remove_cvref_t<Args>>::kFixed && ...)) {
    constexpr auto need = (Codec<std::remove_cvref_t<Args>>::kBytes + ... + std::size_t{0});
    if (uarch::unlikely(cap < need)) return ErrorCode::kRecordTooLarge;
  }
  if constexpr (sizeof...(Args) > 0) {
    template for (constexpr auto index : detail::Indices<sizeof...(Args)>()) {
      using Type = std::remove_cvref_t<decltype(args...[index])>;
      if (const auto status = Codec<Type>::Encode(dest, left, args...[index]); status != ErrorCode::kSuccess) return status;
    }
  }
  return ErrorCode::kSuccess;
}

/**
 * @brief Text decode shared by every call site with the same normalized
 * argument pack. Literals are copied from the segment table; arguments are
 * formatted from the payload cursor.
 */
template <class... Args>
[[nodiscard]] inline ErrorCode DecodeRecord(const abi::StaticRecordInfo *info, [[maybe_unused]] abi::DecodeCursor cursor,
                                            abi::OutputAppender &out) noexcept {
  if (info == nullptr || (info->segment_count != 0 && info->segments == nullptr)) return ErrorCode::kInvalidArgument;
  for (std::uint16_t segment_i = 0; segment_i < info->segment_count; ++segment_i) {
    const abi::FormatSegment &segment = info->segments[segment_i];
    if (segment.kind == abi::SegmentKind::kLiteral) {
      if (const auto status = out.Append(segment.text, segment.text_len); status != ErrorCode::kSuccess) return status;
      continue;
    }
    bool matched = false;
    if constexpr (sizeof...(Args) > 0) {
      template for (constexpr auto index : detail::Indices<sizeof...(Args)>()) {
        if (segment.arg_index == index) {
          matched = true;
          if (const auto status = Codec<Args...[index]>::Format(cursor, segment.presentation, segment.precision, out);
              status != ErrorCode::kSuccess) {
            return status;
          }
        }
      }
    }
    if (!matched) return ErrorCode::kInvalidArgument;
  }
  return ErrorCode::kSuccess;
}

}  // namespace zerolog::encode
