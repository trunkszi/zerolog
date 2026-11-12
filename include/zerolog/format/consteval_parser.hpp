/** @file @brief Compile-time format string parser and promoted StaticRecordInfo builder. */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <zerolog/encode/pack_encoder.hpp>

namespace zerolog::format {

void FormatErrorArityMismatch() noexcept;
void FormatErrorBadSpec() noexcept;
void FormatErrorTypeMismatch() noexcept;
void FormatErrorStrayBrace() noexcept;
void FormatErrorUnclosed() noexcept;
void FormatErrorTooManySegments() noexcept;
void FormatErrorFormatTooLong() noexcept;

namespace detail {

template <class T>
consteval bool SpecAllowed(abi::FormatPresentation presentation) noexcept {
  if (presentation == abi::FormatPresentation::kDefault) return true;
  if (presentation == abi::FormatPresentation::kFixedFloat) return std::is_floating_point_v<T>;
  const bool digits = (std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_enum_v<T>;
  return digits && (presentation == abi::FormatPresentation::kHexLower || presentation == abi::FormatPresentation::kHexUpper ||
                    presentation == abi::FormatPresentation::kBinary);
}

}  // namespace detail

/** @brief Consteval format compiler. The promoted descriptor points at static
 * storage. */
template <abi::Level L, class... Args>
struct FormatString {
  const abi::StaticRecordInfo* info{};

  consteval FormatString(std::string_view text, std::source_location location = std::source_location::current()) {
    static_assert((sizeof...(Args) <= abi::kMaxFormatArgs), "FormatErrorArityMismatch");
    static_assert((!std::is_reference_v<Args> && ...), "FormatErrorTypeMismatch");
    static_assert((!std::is_const_v<Args> && ...), "FormatErrorTypeMismatch");
    static_assert((!std::is_volatile_v<Args> && ...), "FormatErrorTypeMismatch");
    if (text.size() > 4096) FormatErrorFormatTooLong();

    core::InplaceVector<abi::FormatSegment, abi::kMaxFormatSegments> segments;
    char literal[4096]{};
    std::size_t literal_len = 0;
    std::size_t arg_index = 0;
    auto flush = [&]() consteval {
      if (literal_len == 0) return;
      abi::FormatSegment segment{};
      segment.kind = abi::SegmentKind::kLiteral;
      segment.text = uarch::reflect::freeze_string(std::string_view{literal, literal_len}).data();
      segment.text_len = static_cast<std::uint32_t>(literal_len);
      if (segments.try_push_back(segment) == nullptr) FormatErrorTooManySegments();
      literal_len = 0;
    };
    auto push_char = [&](char ch) consteval {
      if (literal_len >= sizeof(literal)) FormatErrorFormatTooLong();
      literal[literal_len++] = ch;
    };

    for (std::size_t cursor = 0; cursor < text.size(); ++cursor) {
      const char ch = text[cursor];
      if (ch == '{') {
        if (cursor + 1 < text.size() && text[cursor + 1] == '{') {
          push_char('{');
          ++cursor;
          continue;
        }
        flush();
        const auto close = text.find('}', cursor + 1);
        if (close == std::string_view::npos) FormatErrorUnclosed();
        const auto spec = text.substr(cursor + 1, close - cursor - 1);
        auto presentation = abi::FormatPresentation::kDefault;
        std::uint8_t precision = 6;
        if (!spec.empty()) {
          if (spec.front() != ':') FormatErrorBadSpec();
          const auto body = spec.substr(1);
          if (body == "x") {
            presentation = abi::FormatPresentation::kHexLower;
          } else if (body == "X") {
            presentation = abi::FormatPresentation::kHexUpper;
          } else if (body == "b") {
            presentation = abi::FormatPresentation::kBinary;
          } else if (body.size() >= 3 && body.front() == '.' && body.back() == 'f') {
            unsigned parsed = 0;
            bool digits = false;
            for (std::size_t i = 1; i + 1 < body.size(); ++i) {
              if (body[i] < '0' || body[i] > '9') FormatErrorBadSpec();
              digits = true;
              parsed = parsed * 10U + static_cast<unsigned>(body[i] - '0');
              if (parsed > 255U) FormatErrorBadSpec();
            }
            if (!digits) FormatErrorBadSpec();
            presentation = abi::FormatPresentation::kFixedFloat;
            precision = static_cast<std::uint8_t>(parsed);
          } else {
            FormatErrorBadSpec();
          }
        }
        if (arg_index >= sizeof...(Args)) FormatErrorArityMismatch();
        if constexpr (sizeof...(Args) > 0) {
          template for (constexpr auto slot : encode::detail::Indices<sizeof...(Args)>()) {
            if (slot == arg_index && !detail::SpecAllowed<Args...[slot]>(presentation)) FormatErrorTypeMismatch();
          }
        }
        abi::FormatSegment segment{};
        segment.kind = abi::SegmentKind::kArg;
        segment.arg_index = static_cast<std::uint16_t>(arg_index);
        segment.presentation = presentation;
        segment.precision = precision;
        if (segments.try_push_back(segment) == nullptr) FormatErrorTooManySegments();
        ++arg_index;
        cursor = close;
        continue;
      }
      if (ch == '}') {
        if (cursor + 1 < text.size() && text[cursor + 1] == '}') {
          push_char('}');
          ++cursor;
          continue;
        }
        FormatErrorStrayBrace();
      }
      push_char(ch);
    }
    flush();
    if (arg_index != sizeof...(Args)) FormatErrorArityMismatch();

    const auto live = segments.size();
    const abi::FormatSegment* promoted_segments = nullptr;
    if (live != 0) {
      const auto promoted = std::define_static_array(std::span<const abi::FormatSegment>{segments.data(), live});
      promoted_segments = promoted.data();
    }
    abi::StaticRecordInfo record{};
    record.decode = &encode::DecodeRecord<Args...>;
    record.segments = promoted_segments;
    record.segment_count = static_cast<std::uint16_t>(live);
    record.arg_count = static_cast<std::uint8_t>(sizeof...(Args));
    record.level = L;
    record.format = uarch::reflect::freeze_string(text).data();
    record.format_len = static_cast<std::uint32_t>(text.size());
    const auto file = std::string_view{location.file_name()};
    const auto function = std::string_view{location.function_name()};
    record.file = uarch::reflect::freeze_string(file).data();
    record.file_len = static_cast<std::uint32_t>(file.size());
    record.function = uarch::reflect::freeze_string(function).data();
    record.function_len = static_cast<std::uint32_t>(function.size());
    record.line = location.line();
    record.column = static_cast<std::uint32_t>(location.column());
    info = std::define_static_array(std::array{record}).data();
  }

  template <std::size_t N>
  consteval FormatString(const char (&text)[N], std::source_location location = std::source_location::current())
      : FormatString(std::string_view{text, (N > 0 && text[N - 1] == '\0') ? (N - 1) : N}, location) {}

  consteval FormatString(const char* text, std::source_location location = std::source_location::current())
      : FormatString(std::string_view{text}, location) {}
};

}  // namespace zerolog::format
