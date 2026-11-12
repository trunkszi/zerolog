/** @file @brief P2996 reflection-based binary codecs for scalars, strings, enums and aggregates. */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <zerolog/abi/record_layout.hpp>
#include <zerolog/format/number_format.hpp>

namespace zerolog::encode {

inline constexpr int kCodecOk = 0;
inline constexpr int kCodecBitField = 1;
inline constexpr int kCodecPrivate = 2;
inline constexpr int kCodecReference = 3;
inline constexpr int kCodecRawPointer = 4;
inline constexpr int kCodecUnion = 5;
inline constexpr int kCodecUnsupported = 6;

namespace detail {

template <std::size_t... Is>
struct IndexStorage {
  static constexpr std::array<std::size_t, sizeof...(Is)> kValues{Is...};
};

template <std::size_t... Is>
consteval std::span<const std::size_t> IndicesImpl(std::index_sequence<Is...>) noexcept {
  return IndexStorage<Is...>::kValues;
}

template <std::size_t N>
consteval std::span<const std::size_t> Indices() noexcept {
  return IndicesImpl(std::make_index_sequence<N>{});
}

[[nodiscard]] inline bool Read(abi::DecodeCursor& cursor, void* dest, std::size_t size) noexcept {
  if (cursor.remaining < size) return false;
  if (size != 0) std::memcpy(dest, cursor.data, size);
  cursor.data += size;
  cursor.remaining -= static_cast<std::uint32_t>(size);
  return true;
}

[[nodiscard, gnu::always_inline]] inline ErrorCode Write(std::byte*& dest, std::size_t& cap, const void* src,
                                                         std::size_t size) noexcept {
  if (uarch::unlikely(cap < size)) return ErrorCode::kRecordTooLarge;
  if (size != 0) std::memcpy(dest, src, size);
  dest += size;
  cap -= size;
  return ErrorCode::kSuccess;
}

template <class T>
[[nodiscard]] constexpr std::uint64_t IntegerKey(T value) noexcept {
  using Unsigned = std::make_unsigned_t<T>;
  return static_cast<std::uint64_t>(static_cast<Unsigned>(value));
}

}  // namespace detail

template <class T>
struct Codec {
  static constexpr int kProblem = kCodecUnsupported;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  static_assert(sizeof(T) == 0, "CodecErrorUnsupported");
};

template <>
struct Codec<bool> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = 1;
  [[nodiscard]] static constexpr std::size_t Size(bool) noexcept { return 1; }
  [[nodiscard, gnu::always_inline]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, bool value) noexcept {
    const auto byte = static_cast<std::uint8_t>(value ? 1 : 0);
    return detail::Write(dest, cap, &byte, 1);
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    std::uint8_t byte = 0;
    if (!detail::Read(cursor, &byte, 1)) return ErrorCode::kInvalidArgument;
    const char* text = byte == 0 ? "false" : "true";
    return out.Append(text, byte == 0 ? 5 : 4);
  }
};

template <class T>
  requires(std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> && sizeof(T) <= 8)
struct Codec<T> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = sizeof(T);
  [[nodiscard]] static constexpr std::size_t Size(T) noexcept { return sizeof(T); }
  [[nodiscard, gnu::always_inline]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, T value) noexcept {
    return detail::Write(dest, cap, &value, sizeof(T));
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation presentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    T value{};
    if (!detail::Read(cursor, &value, sizeof(T))) return ErrorCode::kInvalidArgument;
    const auto bits = detail::IntegerKey(value);
    if (presentation == abi::FormatPresentation::kHexLower) return format::FormatHexLower(out, bits);
    if (presentation == abi::FormatPresentation::kHexUpper) return format::FormatHexUpper(out, bits);
    if (presentation == abi::FormatPresentation::kBinary) return format::FormatBinary(out, bits);
    if constexpr (std::is_signed_v<T>) return format::FormatInt(out, static_cast<std::int64_t>(value));
    return format::FormatUint(out, bits);
  }
};

template <>
struct Codec<char> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = 1;
  [[nodiscard]] static constexpr std::size_t Size(char) noexcept { return 1; }
  [[nodiscard, gnu::always_inline]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, char value) noexcept {
    return detail::Write(dest, cap, &value, 1);
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation presentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    unsigned char value = 0;
    if (!detail::Read(cursor, &value, 1)) return ErrorCode::kInvalidArgument;
    if (presentation == abi::FormatPresentation::kDefault) return out.Push(static_cast<char>(value));
    if (presentation == abi::FormatPresentation::kHexUpper) return format::FormatHexUpper(out, value);
    if (presentation == abi::FormatPresentation::kBinary) return format::FormatBinary(out, value);
    return format::FormatHexLower(out, value);
  }
};

template <>
struct Codec<float> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = sizeof(float);
  [[nodiscard]] static constexpr std::size_t Size(float) noexcept { return sizeof(float); }
  [[nodiscard, gnu::always_inline]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, float value) noexcept {
    return detail::Write(dest, cap, &value, sizeof(float));
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation presentation, std::uint8_t precision,
                                        abi::OutputAppender& out) noexcept {
    float value = 0;
    if (!detail::Read(cursor, &value, sizeof(float))) return ErrorCode::kInvalidArgument;
    return format::FormatFloat(out, value, presentation, precision);
  }
};

template <>
struct Codec<double> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = sizeof(double);
  [[nodiscard]] static constexpr std::size_t Size(double) noexcept { return sizeof(double); }
  [[nodiscard, gnu::always_inline]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, double value) noexcept {
    return detail::Write(dest, cap, &value, sizeof(double));
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation presentation, std::uint8_t precision,
                                        abi::OutputAppender& out) noexcept {
    double value = 0;
    if (!detail::Read(cursor, &value, sizeof(double))) return ErrorCode::kInvalidArgument;
    return format::FormatFloat(out, value, presentation, precision);
  }
};

namespace detail {

[[nodiscard]] inline ErrorCode EncodeBytes(std::byte*& dest, std::size_t& cap, const char* data, std::size_t size) noexcept {
  if (size > 0 && data == nullptr) return ErrorCode::kInvalidArgument;
  if (size > std::numeric_limits<std::uint32_t>::max()) return ErrorCode::kRecordTooLarge;
  const auto length = static_cast<std::uint32_t>(size);
  if (const auto status = Write(dest, cap, &length, sizeof(length)); status != ErrorCode::kSuccess) return status;
  return Write(dest, cap, data, size);
}

[[nodiscard]] inline ErrorCode FormatBytes(abi::DecodeCursor& cursor, abi::OutputAppender& out) noexcept {
  std::uint32_t length = 0;
  if (!Read(cursor, &length, sizeof(length))) return ErrorCode::kInvalidArgument;
  if (cursor.remaining < length) return ErrorCode::kInvalidArgument;
  const auto* text = reinterpret_cast<const char*>(cursor.data);
  cursor.data += length;
  cursor.remaining -= length;
  return out.Append(text, length);
}

}  // namespace detail

template <>
struct Codec<std::string_view> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  [[nodiscard]] static constexpr std::size_t Size(std::string_view value) noexcept {
    return sizeof(std::uint32_t) + value.size();
  }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, std::string_view value) noexcept {
    return detail::EncodeBytes(dest, cap, value.data(), value.size());
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    return detail::FormatBytes(cursor, out);
  }
};

template <>
struct Codec<std::string> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  [[nodiscard]] static constexpr std::size_t Size(const std::string& value) noexcept {
    return sizeof(std::uint32_t) + value.size();
  }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, const std::string& value) noexcept {
    return detail::EncodeBytes(dest, cap, value.data(), value.size());
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    return detail::FormatBytes(cursor, out);
  }
};

template <>
struct Codec<const char*> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  [[nodiscard]] static constexpr std::size_t Size(const char* value) noexcept {
    return Size(value == nullptr ? std::string_view{} : std::string_view{value});
  }
  [[nodiscard]] static constexpr std::size_t Size(std::string_view value) noexcept {
    return sizeof(std::uint32_t) + value.size();
  }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, const char* value) noexcept {
    return Encode(dest, cap, value == nullptr ? std::string_view{} : std::string_view{value});
  }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, std::string_view value) noexcept {
    return detail::EncodeBytes(dest, cap, value.data(), value.size());
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    return detail::FormatBytes(cursor, out);
  }
};

template <>
struct Codec<char*> : Codec<const char*> {
  using Codec<const char*>::Size;
  using Codec<const char*>::Encode;
  [[nodiscard]] static constexpr std::size_t Size(char* value) noexcept { return Codec<const char*>::Size(value); }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, char* value) noexcept {
    return Codec<const char*>::Encode(dest, cap, value);
  }
};

template <class T>
  requires(!std::is_same_v<std::remove_cv_t<T>, char>)
struct Codec<T*> {
  static constexpr int kProblem = kCodecRawPointer;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  static_assert(sizeof(T) == 0, "CodecErrorRawPointer");
};

template <class T>
struct Codec<T&> {
  static constexpr int kProblem = kCodecReference;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  static_assert(sizeof(T) == 0, "CodecErrorReference");
};

template <class T>
struct Codec<T&&> {
  static constexpr int kProblem = kCodecReference;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  static_assert(sizeof(T) == 0, "CodecErrorReference");
};

namespace detail {

struct EnumEntry {
  std::uint64_t key;
  const char* name;
  std::uint32_t name_len;
};

template <class E>
consteval auto BuildEnumTable() {
  constexpr auto count = std::define_static_array(std::meta::enumerators_of(^^E)).size();
  std::array<EnumEntry, count> entries{};
  std::size_t size = 0;
  template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^E))) {
    using Underlying = std::underlying_type_t<E>;
    const auto raw = static_cast<Underlying>([:enumerator:]);
    const auto name = std::meta::identifier_of(enumerator);
    entries[size] =
        EnumEntry{IntegerKey(raw), uarch::reflect::freeze_string(name).data(), static_cast<std::uint32_t>(name.size())};
    ++size;
  }
  for (std::size_t i = 1; i < size; ++i) {
    auto key = entries[i];
    std::size_t hole = i;
    while (hole > 0 && entries[hole - 1].key > key.key) {
      entries[hole] = entries[hole - 1];
      --hole;
    }
    entries[hole] = key;
  }
  return entries;
}

template <class E>
inline constexpr auto kEnumTable = std::define_static_array(BuildEnumTable<E>());

template <class E>
[[nodiscard]] consteval std::uint64_t EnumKeyOf(E value) noexcept {
  return IntegerKey(static_cast<std::underlying_type_t<E>>(value));
}

template <class E>
[[nodiscard]] inline const EnumEntry* FindEnum(std::uint64_t key) noexcept {
  const auto& table = kEnumTable<E>;
  std::size_t lo = 0;
  std::size_t hi = table.size();
  while (lo < hi) {
    const auto mid = lo + ((hi - lo) >> 1);
    if (table[mid].key < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < table.size() && table[lo].key == key) return &table[lo];
  return nullptr;
}

template <class T>
consteval int AuditAggregate();

}  // namespace detail

template <class E>
  requires std::is_enum_v<E>
struct Codec<E> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = sizeof(E);
  [[nodiscard]] static constexpr std::size_t Size(E) noexcept { return sizeof(E); }
  [[nodiscard, gnu::always_inline]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, E value) noexcept {
    return detail::Write(dest, cap, &value, sizeof(E));
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation presentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    E value{};
    if (!detail::Read(cursor, &value, sizeof(value))) return ErrorCode::kInvalidArgument;
    const auto key = detail::IntegerKey(static_cast<std::underlying_type_t<E>>(value));
    if (presentation == abi::FormatPresentation::kDefault) {
      if (const auto* entry = detail::FindEnum<E>(key); entry != nullptr) return out.Append(entry->name, entry->name_len);
      if constexpr (std::is_signed_v<std::underlying_type_t<E>>) {
        return format::FormatInt(out, static_cast<std::int64_t>(static_cast<std::underlying_type_t<E>>(value)));
      }
      return format::FormatUint(out, key);
    }
    if (presentation == abi::FormatPresentation::kHexUpper) return format::FormatHexUpper(out, key);
    if (presentation == abi::FormatPresentation::kBinary) return format::FormatBinary(out, key);
    return format::FormatHexLower(out, key);
  }
};

template <class T>
  requires std::is_union_v<T>
struct Codec<T> {
  static constexpr int kProblem = kCodecUnion;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  static_assert(sizeof(T) == 0, "CodecErrorUnion");
};

template <class T, std::size_t N>
  requires std::is_same_v<std::remove_cv_t<T>, char>
struct Codec<T[N]> {
  static constexpr int kProblem = kCodecOk;
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;

  /** @brief Bytes before the first NUL, or all `N` bytes when none is present. */
  [[nodiscard]] static constexpr std::string_view BoundedStringView(const T (&arr)[N]) noexcept {
    std::size_t length = 0;
    while (length < N && arr[length] != '\0') ++length;
    return {arr, length};
  }
  [[nodiscard]] static constexpr std::size_t Size(const T (&value)[N]) noexcept { return Size(BoundedStringView(value)); }
  [[nodiscard]] static constexpr std::size_t Size(std::string_view value) noexcept {
    return sizeof(std::uint32_t) + value.size();
  }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, const T (&value)[N]) noexcept {
    return Encode(dest, cap, BoundedStringView(value));
  }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, std::string_view value) noexcept {
    return detail::EncodeBytes(dest, cap, value.data(), value.size());
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation, std::uint8_t,
                                        abi::OutputAppender& out) noexcept {
    return detail::FormatBytes(cursor, out);
  }
};

template <class T, std::size_t N>
  requires(!std::is_same_v<std::remove_cv_t<T>, char>)
struct Codec<T[N]> {
  using Element = std::remove_cv_t<T>;
  static constexpr int kProblem = Codec<Element>::kProblem;
  static constexpr bool kFixed = Codec<Element>::kFixed;
  static constexpr std::size_t kBytes = Codec<Element>::kFixed ? N * Codec<Element>::kBytes : 0;
  [[nodiscard]] static constexpr std::size_t Size(const T (&value)[N]) noexcept {
    if constexpr (Codec<Element>::kFixed) return kBytes;
    std::size_t total = 0;
    for (const auto& element : value) total += Codec<Element>::Size(element);
    return total;
  }
  [[nodiscard]] static ErrorCode Encode(std::byte*& dest, std::size_t& cap, const T (&value)[N]) noexcept {
    for (const auto& element : value) {
      if (const auto status = Codec<Element>::Encode(dest, cap, element); status != ErrorCode::kSuccess) return status;
    }
    return ErrorCode::kSuccess;
  }
  [[nodiscard]] static ErrorCode Format(abi::DecodeCursor& cursor, abi::FormatPresentation presentation, std::uint8_t precision,
                                        abi::OutputAppender& out) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
      if (i != 0) {
        if (const auto status = out.Push(','); status != ErrorCode::kSuccess) return status;
      }
      if (const auto status = Codec<Element>::Format(cursor, presentation, precision, out); status != ErrorCode::kSuccess) {
        return status;
      }
    }
    return ErrorCode::kSuccess;
  }
};

template <class T>
  requires(std::is_class_v<T> && !std::is_aggregate_v<T> && !std::is_union_v<T>)
struct Codec<T> {
  static constexpr int kProblem = [] consteval {
    constexpr auto hidden =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^T, uarch::reflect::unchecked_context())).size();
    constexpr auto visible =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())).size();
    if (hidden != visible) return kCodecPrivate;
    return kCodecUnsupported;
  }();
  static constexpr bool kFixed = false;
  static constexpr std::size_t kBytes = 0;
  static_assert(kProblem != kCodecPrivate, "CodecErrorPrivateMember");
  static_assert(kProblem != kCodecUnsupported, "CodecErrorUnsupported");
};

template <class T>
  requires(std::is_class_v<T> && std::is_aggregate_v<T>)
struct Codec<T> {
  static constexpr int kProblem = detail::AuditAggregate<T>();
  static constexpr bool kFixed = [] consteval {
    if constexpr (kProblem != kCodecOk)
      return false;
    else {
      bool fixed = true;
      template for (constexpr auto mem :
                    std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
        using Member = [:std::meta::remove_cv(std::meta::type_of(mem)):];
        fixed = fixed && Codec<Member>::kFixed;
      }
      return fixed;
    }
  }();
  static constexpr std::size_t kBytes = [] consteval {
    if constexpr (!kFixed)
      return std::size_t{0};
    else {
      std::size_t total = 0;
      template for (constexpr auto mem :
                    std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
        using Member = [:std::meta::remove_cv(std::meta::type_of(mem)):];
        total += Codec<Member>::kBytes;
      }
      return total;
    }
  }();
  static_assert(kProblem != kCodecBitField, "CodecErrorBitField");
  static_assert(kProblem != kCodecPrivate, "CodecErrorPrivateMember");
  static_assert(kProblem != kCodecReference, "CodecErrorReference");
  static_assert(kProblem != kCodecRawPointer, "CodecErrorRawPointer");
  static_assert(kProblem != kCodecUnion, "CodecErrorUnion");
  static_assert(kProblem != kCodecUnsupported, "CodecErrorUnsupported");

  [[nodiscard]] static constexpr std::size_t Size([[maybe_unused]] const T& value) noexcept {
    if constexpr (kFixed) return kBytes;
    std::size_t total = 0;
    template for (constexpr auto mem :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
      using Member = [:std::meta::remove_cv(std::meta::type_of(mem)):];
      total += Codec<Member>::Size(value.[:mem:]);
    }
    return total;
  }

  [[nodiscard]] static ErrorCode Encode([[maybe_unused]] std::byte*& dest, [[maybe_unused]] std::size_t& cap,
                                        [[maybe_unused]] const T& value) noexcept {
    template for (constexpr auto mem :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
      using Member = [:std::meta::remove_cv(std::meta::type_of(mem)):];
      if (const auto status = Codec<Member>::Encode(dest, cap, value.[:mem:]); status != ErrorCode::kSuccess) return status;
    }
    return ErrorCode::kSuccess;
  }

  [[nodiscard]] static ErrorCode Format([[maybe_unused]] abi::DecodeCursor& cursor, abi::FormatPresentation, std::uint8_t,
                                        [[maybe_unused]] abi::OutputAppender& out) noexcept {
    bool first = true;
    template for (constexpr auto mem :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
      if (!first) {
        if (const auto status = out.Push(','); status != ErrorCode::kSuccess) return status;
      }
      first = false;
      using Member = [:std::meta::remove_cv(std::meta::type_of(mem)):];
      if (const auto status = Codec<Member>::Format(cursor, abi::FormatPresentation::kDefault, 6, out);
          status != ErrorCode::kSuccess) {
        return status;
      }
    }
    return ErrorCode::kSuccess;
  }
};

namespace detail {

template <class T>
consteval int AuditAggregate() {
  if constexpr (!std::is_class_v<T> || !std::is_aggregate_v<T>) return kCodecUnsupported;
  constexpr auto hidden =
      std::define_static_array(std::meta::nonstatic_data_members_of(^^T, uarch::reflect::unchecked_context())).size();
  constexpr auto visible =
      std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current())).size();
  if (hidden != visible) return kCodecPrivate;
  template for (constexpr auto mem :
                std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
    if constexpr (std::meta::is_bit_field(mem))
      return kCodecBitField;
    else {
      using Member = [:std::meta::remove_cv(std::meta::type_of(mem)):];
      if constexpr (std::is_reference_v<Member>)
        return kCodecReference;
      else if constexpr (std::is_pointer_v<Member>) {
        if constexpr (!std::is_same_v<std::remove_cv_t<std::remove_pointer_t<Member>>, char>) return kCodecRawPointer;
      } else if constexpr (std::is_union_v<Member>) {
        return kCodecUnion;
      } else if constexpr (std::is_array_v<Member>) {
        if constexpr (Codec<std::remove_cv_t<std::remove_extent_t<Member>>>::kProblem != kCodecOk) {
          return Codec<std::remove_cv_t<std::remove_extent_t<Member>>>::kProblem;
        }
      } else if constexpr (Codec<Member>::kProblem != kCodecOk) {
        return Codec<Member>::kProblem;
      }
    }
  }
  return kCodecOk;
}

}  // namespace detail

}  // namespace zerolog::encode
