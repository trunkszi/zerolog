// expected-error: deleted
#include <zerolog/core/lease.hpp>

void CopyConsumerLease(const zerolog::core::ConsumerLease& src) {
  zerolog::core::ConsumerLease copy(src);
  (void)copy;
}
