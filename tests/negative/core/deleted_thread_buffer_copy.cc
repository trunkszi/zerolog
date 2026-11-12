// expected-error: deleted
#include <zerolog/core/thread_buffer.hpp>

void CopyThreadBuffer(const zerolog::core::ThreadBuffer& src) {
  zerolog::core::ThreadBuffer copy(src);
  (void)copy;
}
