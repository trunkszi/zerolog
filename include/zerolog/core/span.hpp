/** @file @brief Thread-local span identifier storage and lexical RAII SpanGuard. */
#pragma once

#include <cstdint>

namespace zerolog::core {

inline constinit thread_local std::uint64_t t_current_span_id = 0U;

/** @brief Return the 64-bit span identifier currently active on the calling thread. */
[[nodiscard, gnu::always_inline]] inline std::uint64_t CurrentSpanId() noexcept { return t_current_span_id; }

/** @brief Lexical non-copyable, non-movable RAII guard binding a 64-bit span identifier on the current thread. */
class SpanGuard final {
 public:
  /** @brief Push @p span_id as the active thread-local span identifier and save the previous one. */
  explicit SpanGuard(std::uint64_t span_id) noexcept : prev_span_id_(t_current_span_id), span_id_(span_id) {
    t_current_span_id = span_id;
  }

  SpanGuard(const SpanGuard&) = delete ("SpanGuard is lexical and non-copyable");
  SpanGuard(SpanGuard&&) = delete ("SpanGuard is lexical and non-movable");
  SpanGuard& operator=(const SpanGuard&) = delete;
  SpanGuard& operator=(SpanGuard&&) = delete;

  /** @brief Restore the previous thread-local span identifier in LIFO order. */
  ~SpanGuard() noexcept { t_current_span_id = prev_span_id_; }

  /** @brief Return the span identifier bound by this guard. */
  [[nodiscard, gnu::always_inline]] std::uint64_t SpanId() const noexcept { return span_id_; }

  /** @brief Return the span identifier that was active before this guard was constructed. */
  [[nodiscard, gnu::always_inline]] std::uint64_t PreviousSpanId() const noexcept { return prev_span_id_; }

 private:
  std::uint64_t prev_span_id_;
  std::uint64_t span_id_;
};

}  // namespace zerolog::core

namespace zerolog {
using core::CurrentSpanId;
using core::SpanGuard;
}  // namespace zerolog
