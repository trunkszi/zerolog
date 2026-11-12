#include <zerolog/core/thread_buffer.hpp>

#include "support/check.hpp"
int main() {
  using namespace zerolog::core;
  CHECK(!ConsumerReentry());
  {
    ReentrancyGuard outer{kInProducerReservation};
    CHECK(outer.Entered() && ConsumerReentry());
    {
      ReentrancyGuard duplicate{kInProducerReservation};
      CHECK(!duplicate.Entered());
    }
    CHECK(HasReentrancyFlag(kInProducerReservation));
    {
      ReentrancyGuard consumer{kInConsumer};
      CHECK(consumer.Entered());
      CHECK(HasReentrancyFlag(kInProducerReservation | kInConsumer));
    }
    CHECK(!HasReentrancyFlag(kInConsumer));
  }
  CHECK(t_reentrancy_flags == kReentrancyNone && !ConsumerReentry());
}
