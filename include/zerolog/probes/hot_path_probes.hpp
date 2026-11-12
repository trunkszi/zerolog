/** @file @brief Tier 1 probe symbols. nullptr from TryReserve means the fast path has no local credit. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace zerolog::core {
class SpscVarQueue;
}

extern "C" {
/** @brief Fast-path reservation. nullptr means the caller must use TryReserveSlow for the ErrorCode. */
[[gnu::noinline]] std::byte* ZerologProbeTryReserve(zerolog::core::SpscVarQueue* queue, std::uint32_t blocks) noexcept;

/** @brief Release-publish one header. A null record is kInvalidArgument. */
[[gnu::noinline]] std::uint32_t ZerologProbePublish(std::byte* record, std::uint64_t header) noexcept;

/**
 * @brief Encode two u64 values.
 * Low 8 bits are ErrorCode. Bits 8..31 are the encoded size when the code is kSuccess.
 */
[[gnu::noinline]] std::uint64_t ZerologProbeInfoU64x2(std::byte* dest, std::uint64_t a, std::uint64_t b) noexcept;

/** @brief Decode two u64 values into caller storage. The return value is ErrorCode. */
[[gnu::noinline]] std::uint32_t ZerologProbeDecodeU64x2(char* dest, std::size_t cap, std::uint64_t a, std::uint64_t b) noexcept;

/** @brief Format one cached header. The return value is ErrorCode. */
[[gnu::noinline]] std::uint32_t ZerologProbeHeaderCacheHit(char* dest, std::size_t cap, std::int64_t unix_ns) noexcept;

/**
 * @brief Forward one completion batch. The concrete ProactorLogSink is parameterized on the host engine,
 * so the caller supplies the thunk. A null thunk is kInvalidArgument.
 */
[[gnu::noinline]] std::uint32_t ZerologProbeSinkOnCompletions(void (*on_completions)(void* sink, const void* first,
                                                                                     std::uint32_t count) noexcept,
                                                              void* sink, const void* first, std::uint32_t count) noexcept;
}
