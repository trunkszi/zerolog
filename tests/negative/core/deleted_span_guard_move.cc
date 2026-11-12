// expected-error: deleted
#include <utility>
#include <zerolog/core/span.hpp>

void MoveSpanGuard() {
  zerolog::core::SpanGuard guard(42U);
  zerolog::core::SpanGuard moved(std::move(guard));
  (void)moved;
}
