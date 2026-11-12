/** @file @brief Unique owners for aligned ring mappings and lazy output slab mappings. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <uring/sys/mmap_region.hpp>
#include <utility>
#include <zerolog/error/zerolog_error.hpp>
namespace zerolog::sys {
inline constexpr std::int32_t kMapNoReserve = 0x4000;
inline constexpr std::int64_t kSysMadvise = 28;
inline constexpr std::uint64_t kMadvHugepage = 14;
/** @brief A 2 MiB aligned view retaining the complete 4 MiB mmap owner. */
class ThpRingMapping final {
  uring::sys::MmapRegion owner_;
  std::span<std::byte> ring_;
  ThpRingMapping(uring::sys::MmapRegion owner, std::span<std::byte> ring) noexcept : owner_(std::move(owner)), ring_(ring) {}

 public:
  ThpRingMapping(const ThpRingMapping&) = delete;
  ThpRingMapping& operator=(const ThpRingMapping&) = delete;
  ThpRingMapping(ThpRingMapping&& other) noexcept : owner_(std::move(other.owner_)), ring_(std::exchange(other.ring_, {})) {}
  ThpRingMapping& operator=(ThpRingMapping&& other) noexcept {
    if (this != &other) {
      owner_ = std::move(other.owner_);
      ring_ = std::exchange(other.ring_, {});
    }
    return *this;
  }
  /** @brief Allocate, advise and prefault the aligned ring view. */
  [[nodiscard]] static Result<ThpRingMapping> Create() noexcept {
    constexpr std::size_t size = uarch::PowerOfTwo<21>::value;
    auto owner = uring::sys::MmapRegion::Map(nullptr, size * 2, uring::sys::kProtRead | uring::sys::kProtWrite,
                                             uring::sys::kMapPrivate | uring::sys::kMapAnonymous | kMapNoReserve, -1, 0);
    if (!owner) return std::unexpected(FromUring(owner.error()));
    const auto base = reinterpret_cast<std::uintptr_t>(owner->bytes().data());
    const auto aligned = uarch::PowerOfTwo<21>::checked_align_up(base);
    if (!aligned) return std::unexpected(ErrorCode::kInvalidArgument);
    const auto offset = *aligned - base;
    auto ring = owner->bytes().subspan(offset, size);
    // Retain the original full mapping: unmapping only the interior would split its owner.
    (void)uring::sys::Syscall6(kSysMadvise, uring::sys::Address(ring.data()), ring.size(), kMadvHugepage);
    for (std::size_t i = 0; i < ring.size(); i += 4096) *static_cast<volatile std::byte*>(ring.data() + i) = std::byte{};
    return ThpRingMapping{std::move(*owner), ring};
  }
  /** @brief Borrow usable bytes for at most the lifetime of this owner. */
  [[nodiscard]] std::span<std::byte> Bytes() const noexcept { return ring_; }
  /** @brief Full owned mapping extent, including alignment slack. */
  [[nodiscard]] std::size_t MappedSize() const noexcept { return owner_.bytes().size(); }
};
/** @brief One lazy 16 MiB mapping; OutputBuffer owns one per slab. */
class SlabMapping final {
  uring::sys::MmapRegion owner_;
  explicit SlabMapping(uring::sys::MmapRegion owner) noexcept : owner_(std::move(owner)) {}

 public:
  SlabMapping(SlabMapping&&) noexcept = default;
  SlabMapping& operator=(SlabMapping&&) noexcept = default;
  /** @brief Reserve an output slab without eager page commitment. */
  [[nodiscard]] static Result<SlabMapping> Create() noexcept {
    auto owner =
        uring::sys::MmapRegion::Map(nullptr, uarch::PowerOfTwo<24>::value, uring::sys::kProtRead | uring::sys::kProtWrite,
                                    uring::sys::kMapPrivate | uring::sys::kMapAnonymous | kMapNoReserve, -1, 0);
    if (!owner) return std::unexpected(FromUring(owner.error()));
    return SlabMapping{std::move(*owner)};
  }
  /** @brief Borrow usable bytes for at most the lifetime of this owner. */
  [[nodiscard]] std::span<std::byte> Bytes() const noexcept { return owner_.bytes(); }
};
}  // namespace zerolog::sys
