#include <zerolog/core/scratch_heap.hpp>

#include "support/check.hpp"
int main() {
  zerolog::core::ScratchHeap heap;
  CHECK(!heap.Pop() && heap.Pop().error() == zerolog::ErrorCode::kWouldBlock);
  CHECK(heap.TryPush({0, 64, 4}) == zerolog::ErrorCode::kInvalidArgument);
  for (std::uint32_t slot = 0; slot < 64; ++slot) CHECK(heap.TryPush({1000 - slot, slot, 4}) == zerolog::ErrorCode::kSuccess);
  CHECK(heap.Size() == 64 && heap.TryPush({0, 0, 4}) == zerolog::ErrorCode::kQueueFull);
  for (std::uint32_t n = 0; n < 64; ++n) {
    const auto node = heap.Pop();
    CHECK(node && node->slot == 63 - n && node->tsc == 937 + n);
    CHECK(!heap.Contains(node->slot));
  }
  CHECK(heap.Empty());
  CHECK(heap.TryPush({10, 4, 4}) == zerolog::ErrorCode::kSuccess);
  CHECK(heap.TryPush({9, 4, 4}) == zerolog::ErrorCode::kInvalidArgument);
  CHECK(heap.TryPush({10, 2, 4}) == zerolog::ErrorCode::kSuccess);
  CHECK(heap.Pop()->slot == 2 && heap.Pop()->slot == 4);
}
