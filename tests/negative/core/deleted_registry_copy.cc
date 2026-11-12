// expected-error: deleted
#include <zerolog/core/registry.hpp>

void CopyRegistry(const zerolog::core::ThreadRegistry& src) {
  zerolog::core::ThreadRegistry copy(src);
  (void)copy;
}
