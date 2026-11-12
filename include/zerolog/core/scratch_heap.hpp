/** @file @brief Bounded four-ary timestamp merge heap with one retained head per registry slot. */
#pragma once
#include <optional>
#include <zerolog/abi/contract.hpp>
#include <zerolog/error/zerolog_error.hpp>
namespace zerolog::core {
/** @brief A consumer-owned merge frontier; no registry slot may have two nodes. */
class ScratchHeap final {
 public:
  /** @brief Reject invalid slots, duplicate heads or exhausted capacity without mutation. */
  [[nodiscard]] ErrorCode TryPush(abi::HeapNode node) noexcept {
    if (node.slot >= 64) return ErrorCode::kInvalidArgument;
    if (nodes_.full()) return ErrorCode::kQueueFull;
    if (Contains(node.slot)) return ErrorCode::kInvalidArgument;
    auto index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.unchecked_push_back(node);
    mask_ = mask_.set(node.slot);
    while (index != 0) {
      const auto parent = (index - 1) >> 2;
      if (!Less(node, nodes_.data()[parent])) break;
      nodes_.data()[index] = nodes_.data()[parent];
      index = parent;
    }
    nodes_.data()[index] = node;
    return ErrorCode::kSuccess;
  }
  /** @brief Remove the oldest head; an empty frontier reports WouldBlock. */
  [[nodiscard]] Result<abi::HeapNode> Pop() noexcept {
    if (nodes_.empty()) return std::unexpected(ErrorCode::kWouldBlock);
    const auto result = nodes_.data()[0];
    mask_ = mask_.clear(result.slot);
    const auto last = nodes_.data()[nodes_.size() - 1];
    nodes_.pop_back();
    if (nodes_.empty()) return result;
    nodes_.data()[0] = last;
    std::uint32_t index = 0;
    for (;;) {
      const auto first = index * 4 + 1;
      if (first >= nodes_.size()) break;
      auto best = first;
      for (std::uint32_t child = first + 1; child < first + 4 && child < nodes_.size(); ++child)
        best = uarch::predicate(Less(nodes_.data()[child], nodes_.data()[best])).select_register(child, best);
      if (!Less(nodes_.data()[best], last)) break;
      nodes_.data()[index] = nodes_.data()[best];
      index = best;
    }
    nodes_.data()[index] = last;
    return result;
  }
  /** @brief Observe a head without relinquishing its slot retention. */
  [[nodiscard]] std::optional<abi::HeapNode> Peek() const noexcept {
    if (nodes_.empty()) return std::nullopt;
    return nodes_.data()[0];
  }
  [[nodiscard]] bool Contains(std::uint32_t slot) const noexcept { return slot < 64 && mask_.test(slot); }
  [[nodiscard]] std::size_t Size() const noexcept { return nodes_.size(); }
  [[nodiscard]] bool Empty() const noexcept { return nodes_.empty(); }

 private:
  [[nodiscard]] static bool Less(abi::HeapNode left, abi::HeapNode right) noexcept {
    return uarch::eager_any(left.tsc < right.tsc, uarch::eager_all(left.tsc == right.tsc, left.slot < right.slot));
  }
  InplaceVector<abi::HeapNode, 64> nodes_; /**< Inline storage for exactly one head per producer. */
  uarch::Bitmap64 mask_;                   /**< Retained slots cannot be reclaimed by the consumer's registry scan. */
};
}  // namespace zerolog::core
