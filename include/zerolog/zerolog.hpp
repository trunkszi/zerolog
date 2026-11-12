/** @file @brief Public facade. */
#pragma once

#include <zerolog/abi/contract.hpp>
#include <zerolog/abi/record_layout.hpp>
#include <zerolog/core/consumer.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/producer.hpp>
#include <zerolog/core/registry.hpp>
#include <zerolog/core/span.hpp>
#include <zerolog/core/spsc_var_queue.hpp>
#include <zerolog/encode/codec.hpp>
#include <zerolog/encode/pack_encoder.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/format/consteval_parser.hpp>
#include <zerolog/format/header_cache.hpp>
#include <zerolog/format/number_format.hpp>
#include <zerolog/sink/futex_fallback.hpp>
#include <zerolog/sink/proactor_sink.hpp>
#include <zerolog/sink/rotation.hpp>
#include <zerolog/sink/worker_thread.hpp>

namespace zerolog {
using abi::DrainBudget;
using abi::DrainStats;
using abi::Level;
using abi::QueueFullPolicy;
using core::CurrentSpanId;
using core::FlushOn;
using core::GetFlushLevel;
using core::GetLevel;
using core::GetQueueFullCallback;
using core::GetQueueFullPolicy;
using core::QueueFullCallback;
using core::RetireCurrentThread;
using core::SetLevel;
using core::SetQueueFullCallback;
using core::SetQueueFullPolicy;
using core::SpanGuard;
using core::WriteCallback;
using sink::Builder;
using sink::FutexFallbackSink;
using sink::LoggerGuard;
using sink::ProactorLogSink;
using sink::SinkConfig;
using sink::UringConsumerThread;
using sink::WorkerBackend;
using sink::WorkerConfig;

inline constexpr Level kTrace = Level::kTrace;
inline constexpr Level kDebug = Level::kDebug;
inline constexpr Level kInfo = Level::kInfo;
inline constexpr Level kWarn = Level::kWarn;
inline constexpr Level kError = Level::kError;
inline constexpr Level kOff = Level::kOff;

/** @brief Claim and prefault this thread's SPSC ring, returning the exact Result<ThreadBuffer*>. */
[[nodiscard]] inline Result<core::ThreadBuffer*> TryPrewarmThread() noexcept { return core::PrewarmCurrentThread(); }

/** @brief Claim and prefault this thread's SPSC ring as a fire-and-forget statement at thread start. */
inline void PrewarmThread() noexcept { [[maybe_unused]] const auto status = core::PrewarmCurrentThread(); }

/** @brief Set the calling thread's diagnostic name (up to 31 bytes) and return the exact Result<void>. */
[[nodiscard]] inline Result<void> TrySetThreadName(std::string_view name) noexcept {
  return core::TrySetCurrentThreadName(name);
}

/** @brief Set the calling thread's diagnostic name (up to 31 bytes) as a fire-and-forget statement. */
inline void SetThreadName(std::string_view name) noexcept {
  [[maybe_unused]] const auto status = core::TrySetCurrentThreadName(name);
}

/** @brief Synchronously drain and flush all pending log records in Mode C without requiring caller status inspection. */
inline void Flush() noexcept { [[maybe_unused]] const auto status = core::TryPoll(true); }
}  // namespace zerolog
