/** @file @brief Function-style producer entry points with explicit runtime errors and snapshot encoding. */
#pragma once
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>
#include <zerolog/core/registry.hpp>
#include <zerolog/core/span.hpp>
#include <zerolog/core/thread_buffer.hpp>
#include <zerolog/encode/pack_encoder.hpp>
#include <zerolog/format/consteval_parser.hpp>
#include <zerolog/sys/tsc_clock.hpp>
namespace zerolog::core {
/** @brief Evaluate an argument only after its message passes level filtering. */
template <class F>
struct Lazy {
  F function;
  [[nodiscard]] decltype(auto) Evaluate() const noexcept {
    static_assert(std::is_nothrow_invocable_v<const F&>);
    return std::invoke(function);
  }
};
template <class F>
Lazy(F) -> Lazy<F>;
namespace detail {
[[nodiscard, gnu::cold, gnu::noinline]] inline Result<ThreadBuffer*> ProducerRegisterSlow() noexcept {
  return PrewarmCurrentThread();
}

template <class T>
struct IsLazy : std::false_type {};
template <class F>
struct IsLazy<Lazy<F>> : std::true_type {};
template <class T>
[[nodiscard, gnu::always_inline]] inline decltype(auto) Prepare(const T& value) noexcept {
  if constexpr (IsLazy<std::remove_cvref_t<T>>::value) {
    using Value = std::remove_cvref_t<decltype(value.Evaluate())>;
    if constexpr (std::is_array_v<Value>)
      return value.Evaluate();
    else
      return Value(value.Evaluate());
  } else
    return static_cast<const T&>(value);
}
/** @brief Bound every addition before a malicious or overflowing custom size can wrap the total. */
template <class... Prepared>
[[nodiscard, gnu::always_inline]] inline bool PayloadSize(std::size_t& size, const Prepared&... prepared) noexcept {
  constexpr std::size_t limit = abi::kMaxRecordBlocks * abi::Block::value - sizeof(abi::RecordPrefix);
  size = 0;
  return ([&]() noexcept {
    const auto bytes = encode::EncodedBytes(prepared);
    if (bytes > limit - size) return false;
    size += bytes;
    return true;
  }() && ...);
}

template <abi::Level L, class... Args>
[[nodiscard, gnu::cold, gnu::noinline]] ErrorCode RegisteredLogCold(
    format::FormatString<L, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt, uarch::ColdArg<Args>... args) noexcept;

[[nodiscard, gnu::cold, gnu::noinline]] inline ErrorCode NotifyUrgentSuccessCold() noexcept {
  RingDoorbellUrgentCold();
  auto status = ErrorCode::kSuccess;
  // Keep the return value opaque to IPA so notification remains a tail call.
  asm("" : "+r"(status));
  return status;
}
[[nodiscard, gnu::cold, gnu::noinline]] inline ErrorCode NotifyNormalSuccessCold() noexcept {
  RingDoorbellCold();
  auto status = ErrorCode::kSuccess;
  asm("" : "+r"(status));
  return status;
}
template <class... Prepared>
[[nodiscard, gnu::always_inline]] inline ErrorCode EncodePayload(std::byte* record, const abi::StaticRecordInfo* info,
                                                                 std::uint32_t payload_bytes,
                                                                 const Prepared&... values) noexcept {
  const auto timestamp = sys::ReadTsc();
  const auto span = CurrentSpanId();
  // Separate word copies preserve header atomicity and avoid a stack-materialized prefix on GCC.
  std::memcpy(record + 8, &info, sizeof(const abi::StaticRecordInfo*));
  std::memcpy(record + 16, &timestamp, sizeof(timestamp));
  std::memcpy(record + 24, &span, sizeof(span));
  return encode::EncodeRecord(record + sizeof(abi::RecordPrefix), payload_bytes, values...);
}
template <abi::Level L, class... Prepared>
[[nodiscard, gnu::always_inline]] inline ErrorCode EncodeReserved(ThreadBuffer& buf, std::byte* record,
                                                                  const abi::StaticRecordInfo* info, std::uint32_t blocks,
                                                                  std::uint32_t payload_bytes,
                                                                  const Prepared&... values) noexcept {
  ErrorCode status;
  // Built-in scalar codecs only copy bytes; custom fixed-size codecs may still fail or re-enter.
  if constexpr ((std::is_arithmetic_v<Prepared> && ...)) {
    status = EncodePayload(record, info, payload_bytes, values...);
  } else {
    ScopedReentrancyFlag guard{kReentrancyInProducerReservation};
    status = EncodePayload(record, info, payload_bytes, values...);
  }
  if (uarch::unlikely(status != ErrorCode::kSuccess)) {
    buf.CancelReservation(record, blocks);
    return status;
  }
  auto payload = payload_bytes;
  // A register dependency avoids materializing the high header field as movabs.
  asm("" : "+r"(payload));
  const auto header = abi::RecordHeaderWord{}
                          .with<^^abi::RecordHeaderSpec::blocks>(blocks)
                          .with<^^abi::RecordHeaderSpec::level>(L)
                          .with<^^abi::RecordHeaderSpec::payload_bytes>(payload);
  SpscVarQueue::Publish(record, header);
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const auto fresh = LoadLevelGate(std::memory_order_relaxed);
  const bool watermark_notify = buf.ConsumeWatermarkNotify();
  if (uarch::unlikely(watermark_notify)) return NotifyUrgentSuccessCold();
  if (uarch::unlikely(L >= fresh.get<^^abi::LevelGateSpec::ring_level>())) return NotifyNormalSuccessCold();
  return ErrorCode::kSuccess;
}

template <abi::Level L, class... Prepared>
[[nodiscard, gnu::cold, gnu::noinline]] ErrorCode ReserveAndEncodeCold(ThreadBuffer* buf, const abi::StaticRecordInfo* info,
                                                                       std::uint32_t blocks, std::uint32_t payload,
                                                                       uarch::ColdArg<Prepared>... values) noexcept {
  auto reserved = buf->TryReserveSlow(blocks, L);
  if (!reserved) return reserved.error();
  return EncodeReserved<L>(*buf, *reserved, info, blocks, payload, values...);
}
template <abi::Level L, class... Normalized>
[[nodiscard, gnu::always_inline]] inline ErrorCode LogNormalized(ThreadBuffer* buf, const abi::StaticRecordInfo* info,
                                                                 const Normalized&... values) noexcept {
  std::size_t payload{};
  if (!PayloadSize(payload, values...)) return ErrorCode::kRecordTooLarge;
  const auto blocks = static_cast<std::uint32_t>((sizeof(abi::RecordPrefix) + payload + 7) >> 3);
  auto* record = buf->TryReserve(blocks);
  if (uarch::unlikely(record == nullptr))
    return ReserveAndEncodeCold<L, Normalized...>(buf, info, blocks, static_cast<std::uint32_t>(payload), values...);
  return EncodeReserved<L>(*buf, record, info, blocks, static_cast<std::uint32_t>(payload), values...);
}
template <abi::Level L, class... Prepared>
[[nodiscard, gnu::always_inline]] inline ErrorCode LogPrepared(ThreadBuffer* buf, const abi::StaticRecordInfo* info,
                                                               const Prepared&... evaluated) noexcept {
  return LogNormalized<L>(buf, info, encode::PrepareArg(evaluated)...);
}
}  // namespace detail
inline constexpr auto kCompiledMinLevel = abi::Level::kTrace;
/** @brief Encode one admitted record; failures propagate as ErrorCode without publishing or losing credit. */
template <abi::Level L, class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode TryLog(
    format::FormatString<L, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt, const Args&... args) noexcept {
  if constexpr (L < kCompiledMinLevel || L == abi::Level::kOff) return ErrorCode::kSuccess;
  if constexpr (L > abi::Level::kOff) return ErrorCode::kInvalidArgument;
  const auto gate = LoadLevelGate(std::memory_order_relaxed);
  if (L < gate.get<^^abi::LevelGateSpec::min_level>()) return ErrorCode::kSuccess;
  if (uarch::unlikely(HasReentrancyFlag(kInProducerReservation))) return ErrorCode::kConsumerBusy;
  if (uarch::unlikely(t_thread_exiting)) return ErrorCode::kShuttingDown;
  if (uarch::unlikely(fmt.info == nullptr)) return ErrorCode::kInvalidArgument;
  auto* buf = t_buffer;
  if (uarch::unlikely(buf == nullptr)) return detail::RegisteredLogCold<L, Args...>(fmt, args...);
  if constexpr ((!detail::IsLazy<std::remove_cvref_t<Args>>::value && ...))
    return detail::LogNormalized<L>(buf, fmt.info, encode::PrepareArg(args)...);
  else
    return detail::LogPrepared<L>(buf, fmt.info, detail::Prepare(args)...);
}
namespace detail {
template <abi::Level L, class... Args>
[[nodiscard, gnu::cold, gnu::noinline]] ErrorCode RegisteredLogCold(
    format::FormatString<L, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt, uarch::ColdArg<Args>... args) noexcept {
  const auto registered = ProducerRegisterSlow();
  if (!registered) return registered.error();
  return TryLog<L, Args...>(fmt, args...);
}
}  // namespace detail
/** @brief Log at compile-time severity @p L without requiring caller status inspection. */
template <abi::Level L, class... Args>
[[gnu::always_inline]] inline void Log(format::FormatString<L, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
                                       const Args&... args) noexcept {
  (void)TryLog<L>(fmt, args...);
}
/** @brief Execute a callback only when its requested level is enabled. */
template <class F>
ErrorCode IfEnabled(abi::Level level, F&& function) noexcept {
  static_assert(std::is_nothrow_invocable_v<F>);
  if (level > abi::Level::kOff) return ErrorCode::kInvalidArgument;
  if (level == abi::Level::kOff || level < kCompiledMinLevel || level < GetLevel()) return ErrorCode::kSuccess;
  if constexpr (std::is_same_v<std::invoke_result_t<F>, ErrorCode>)
    return std::invoke(std::forward<F>(function));
  else {
    static_assert(std::is_void_v<std::invoke_result_t<F>>);
    std::invoke(std::forward<F>(function));
    return ErrorCode::kSuccess;
  }
}
/** @brief Log at trace severity and return the exact ErrorCode. */
template <class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode TryTrace(
    format::FormatString<abi::Level::kTrace, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  return TryLog<abi::Level::kTrace>(fmt, args...);
}
/** @brief Log at debug severity and return the exact ErrorCode. */
template <class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode TryDebug(
    format::FormatString<abi::Level::kDebug, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  return TryLog<abi::Level::kDebug>(fmt, args...);
}
/** @brief Log at info severity and return the exact ErrorCode. */
template <class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode TryInfo(
    format::FormatString<abi::Level::kInfo, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  return TryLog<abi::Level::kInfo>(fmt, args...);
}
/** @brief Log at warn severity and return the exact ErrorCode. */
template <class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode TryWarn(
    format::FormatString<abi::Level::kWarn, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  return TryLog<abi::Level::kWarn>(fmt, args...);
}
/** @brief Log at error severity and return the exact ErrorCode. */
template <class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode TryError(
    format::FormatString<abi::Level::kError, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  return TryLog<abi::Level::kError>(fmt, args...);
}
/** @brief Log at fatal/error severity and return the exact ErrorCode. */
template <class... Args>
[[nodiscard, gnu::always_inline]] inline ErrorCode TryFatal(
    format::FormatString<abi::Level::kError, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  return TryLog<abi::Level::kError>(fmt, args...);
}
/** @brief Log at trace severity. */
template <class... Args>
[[gnu::always_inline]] inline void Trace(
    format::FormatString<abi::Level::kTrace, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  (void)TryLog<abi::Level::kTrace>(fmt, args...);
}
/** @brief Log at debug severity. */
template <class... Args>
[[gnu::always_inline]] inline void Debug(
    format::FormatString<abi::Level::kDebug, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  (void)TryLog<abi::Level::kDebug>(fmt, args...);
}
/** @brief Log at info severity. */
template <class... Args>
[[gnu::always_inline]] inline void Info(
    format::FormatString<abi::Level::kInfo, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  (void)TryLog<abi::Level::kInfo>(fmt, args...);
}
/** @brief Log at warn severity. */
template <class... Args>
[[gnu::always_inline]] inline void Warn(
    format::FormatString<abi::Level::kWarn, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  (void)TryLog<abi::Level::kWarn>(fmt, args...);
}
/** @brief Log at error severity. */
template <class... Args>
[[gnu::always_inline]] inline void Error(
    format::FormatString<abi::Level::kError, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  (void)TryLog<abi::Level::kError>(fmt, args...);
}
/** @brief Log at fatal/error severity without terminating the process. */
template <class... Args>
[[gnu::always_inline]] inline void Fatal(
    format::FormatString<abi::Level::kError, std::type_identity_t<std::remove_cvref_t<Args>>...> fmt,
    const Args&... args) noexcept {
  (void)TryLog<abi::Level::kError>(fmt, args...);
}
}  // namespace zerolog::core
namespace zerolog::encode {
/** @brief Lazy descriptors decode the evaluated value's snapshot representation. */
template <class F>
struct Codec<core::Lazy<F>> : Codec<std::remove_cvref_t<std::invoke_result_t<const F&>>> {};
}  // namespace zerolog::encode
namespace zerolog {
using core::Debug;
using core::Error;
using core::Fatal;
using core::IfEnabled;
using core::Info;
using core::Lazy;
using core::Log;
using core::Trace;
using core::TryDebug;
using core::TryError;
using core::TryFatal;
using core::TryInfo;
using core::TryLog;
using core::TryTrace;
using core::TryWarn;
using core::Warn;
}  // namespace zerolog
