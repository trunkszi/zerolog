/** @file @brief Per-thread 2 MiB variable-length SPSC ring buffer and 16-byte record views. */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <uarch/uarch.hpp>
#include <utility>
#include <zerolog/abi/contract.hpp>
#include <zerolog/abi/record_layout.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/sys/thp_mmap.hpp>

namespace zerolog::core {

/** @brief Register-passable 16-byte producer reservation descriptor. */
struct Reservation {
  std::byte* record{nullptr};            /**< Pointer to reserved 8-byte-aligned record slot. */
  std::uint32_t blocks{0};               /**< Reserved extent in 8-byte blocks. */
  std::uint32_t notify_after_publish{0}; /**< Non-zero if watermark crossed during reservation. */
};
static_assert(sizeof(Reservation) == 16 && uarch::NoPadding<Reservation> && uarch::GprPair<Reservation>);

/** @brief Register-passable 16-byte consumer view over a published ring record. */
struct RecordView {
  const std::byte* data{nullptr}; /**< Base pointer of the published record in the SPSC ring. */
  abi::RecordHeaderWord header{}; /**< Acquire-loaded 64-bit record header word. */

  /** @brief Return true when this view references a valid published record. */
  [[nodiscard, gnu::always_inline]] constexpr bool Valid() const noexcept {
    return data != nullptr && header.get<^^abi::RecordHeaderSpec::blocks>() >= 4U;
  }

  /** @brief Access the 32-byte record prefix at the start of the record. */
  [[nodiscard, gnu::always_inline]] const abi::RecordPrefix* Prefix() const noexcept {
    return reinterpret_cast<const abi::RecordPrefix*>(data);
  }

  /** @brief Return a decode cursor positioned immediately after the 32-byte prefix. */
  [[nodiscard, gnu::always_inline]] abi::DecodeCursor Cursor() const noexcept {
    return abi::DecodeCursor{data + sizeof(abi::RecordPrefix), header.get<^^abi::RecordHeaderSpec::payload_bytes>(), 0U};
  }
};
static_assert(sizeof(RecordView) == 16 && uarch::NoPadding<RecordView> && uarch::GprPair<RecordView>);

/** @brief Per-thread 2 MiB variable-length single-producer single-consumer ring queue. */
class SpscVarQueue final {
 public:
  static constexpr auto kRingCap = static_cast<std::uint32_t>(abi::RingBlocks::value);
  static constexpr std::uint32_t kHalfRing = kRingCap / 2U;

  SpscVarQueue(const SpscVarQueue&) = delete;
  SpscVarQueue& operator=(const SpscVarQueue&) = delete;

  /** @brief Move-construct an SPSC queue and transfer ownership of its 2 MiB THP mapping. */
  SpscVarQueue(SpscVarQueue&& other) noexcept
      : base_(std::exchange(other.base_, nullptr)),
        producer_{other.producer_.value},
        read_idx_{other.read_idx_->load(std::memory_order_relaxed)},
        consumer_idx_{other.consumer_idx_.value},
        mapping_(std::move(other.mapping_)) {}

  /** @brief Move-assign an SPSC queue and transfer ownership of its 2 MiB THP mapping. */
  SpscVarQueue& operator=(SpscVarQueue&& other) noexcept {
    if (this != &other) {
      base_ = std::exchange(other.base_, nullptr);
      producer_.value = other.producer_.value;
      read_idx_->store(other.read_idx_->load(std::memory_order_relaxed), std::memory_order_relaxed);
      consumer_idx_.value = other.consumer_idx_.value;
      mapping_ = std::move(other.mapping_);
    }
    return *this;
  }

  /** @brief Allocate and initialize a 2 MiB THP-backed SPSC variable-length ring. */
  [[nodiscard]] static Result<SpscVarQueue> Create() noexcept {
    auto mapping = sys::ThpRingMapping::Create();
    if (!mapping) {
      return std::unexpected(mapping.error());
    }
    return SpscVarQueue{std::move(*mapping)};
  }

  /** @brief Fast-path producer reservation using local free-block credit without atomic RMW. */
  [[nodiscard, gnu::always_inline]] std::byte* TryReserve(std::uint32_t blocks) noexcept {
    const auto meta = producer_.value;
    if (uarch::unlikely(blocks > meta.get<^^abi::ProducerMetaSpec::free_blocks>())) {
      return nullptr;
    }
    const auto idx = meta.get<^^abi::ProducerMetaSpec::write_idx>();
    // Prevent InstCombine from folding (b << 8) - (b << 32) into a 64-bit movabs multiplier.
    std::uint64_t b = blocks;
    if !consteval {
      asm("" : "+r"(b));
    }
    std::uint64_t next_raw = meta.raw() + (b << 8);
    if !consteval {
      asm("" : "+r"(next_raw));
    }
    next_raw -= (b << 32);
    producer_.value = abi::ProducerMetaWord::from_raw(next_raw);
    return base_ + (static_cast<std::size_t>(idx) << 3);
  }

  /** @brief Slow-path producer reservation refreshing consumer progress and returning exact ErrorCode on failure. */
  [[nodiscard, gnu::cold, gnu::noinline]] Result<std::byte*> TryReserveSlow(std::uint32_t blocks) noexcept {
    if (blocks == 0U || blocks > abi::kMaxRecordBlocks) {
      return std::unexpected(ErrorCode::kRecordTooLarge);
    }
    const auto meta = producer_.value;
    std::uint32_t w = meta.get<^^abi::ProducerMetaSpec::write_idx>();
    bool rung = meta.test<^^abi::ProducerMetaSpec::watermark_rung>();
    const std::uint32_t r = read_idx_->load(std::memory_order_acquire);

    const std::uint32_t pre_occupied = (w >= r) ? (w - r) : (kRingCap - r + w);
    if (pre_occupied < (kRingCap / 4U)) {
      rung = false;
    }

    std::uint32_t avail = 0U;
    std::uint32_t occ_before = pre_occupied;
    if (w >= r) {
      const std::uint32_t avail_tail = (kRingCap - 1U) - w;
      if (blocks <= avail_tail) {
        avail = avail_tail;
      } else {
        if (r <= blocks) {
          producer_.value = meta.with<^^abi::ProducerMetaSpec::watermark_rung>(rung)
                                .with<^^abi::ProducerMetaSpec::notify_after_publish>(false)
                                .with<^^abi::ProducerMetaSpec::free_blocks>(0U);
          return std::unexpected(ErrorCode::kQueueFull);
        }
        const auto wrap_raw = abi::RecordHeaderWord{}.with<^^abi::RecordHeaderSpec::blocks>(abi::kWrapSentinel).raw();
        auto* sentinel_ptr = reinterpret_cast<std::uint64_t*>(base_ + (static_cast<std::size_t>(w) << 3));
        std::atomic_ref<std::uint64_t>{*sentinel_ptr}.store(wrap_raw, std::memory_order_release);
        w = 0U;
        avail = r - 1U;
        occ_before = kRingCap - r;
      }
    } else {
      avail = (r - 1U) - w;
      if (blocks > avail) {
        producer_.value = meta.with<^^abi::ProducerMetaSpec::watermark_rung>(rung)
                              .with<^^abi::ProducerMetaSpec::notify_after_publish>(false)
                              .with<^^abi::ProducerMetaSpec::free_blocks>(0U);
        return std::unexpected(ErrorCode::kQueueFull);
      }
    }

    const std::uint32_t post_occupied = occ_before + blocks;
    std::uint32_t credit_batch = std::min(avail, std::max(abi::kMaxRecordBlocks, blocks));
    bool notify = meta.test<^^abi::ProducerMetaSpec::notify_after_publish>();

    if (!rung) {
      if (post_occupied >= kHalfRing) {
        rung = true;
        notify = true;
      } else {
        const std::uint32_t max_before_half = (kHalfRing - 1U) - occ_before;
        credit_batch = std::min(credit_batch, std::max(blocks, max_before_half));
      }
    }

    producer_.value = abi::ProducerMetaWord{}
                          .with<^^abi::ProducerMetaSpec::watermark_rung>(rung)
                          .with<^^abi::ProducerMetaSpec::notify_after_publish>(notify)
                          .with<^^abi::ProducerMetaSpec::write_idx>(w + blocks)
                          .with<^^abi::ProducerMetaSpec::free_blocks>(credit_batch - blocks);
    return base_ + (static_cast<std::size_t>(w) << 3);
  }

  /** @brief Slow-path producer reservation returning nullptr on failure for hot-to-cold call sites. */
  [[nodiscard, gnu::cold, gnu::noinline]] std::byte* ReserveSlow(std::uint32_t blocks) noexcept {
    auto res = TryReserveSlow(blocks);
    return res ? *res : nullptr;
  }

  /** @brief Roll back an uncommitted producer reservation and scrub its blocks to zero. */
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[gnu::cold, gnu::noinline]] void CancelReservation(std::byte* record, std::uint32_t blocks) noexcept {
    if (record == nullptr || blocks == 0U) {
      return;
    }
    auto* word_ptr = reinterpret_cast<std::uint64_t*>(record);
    std::atomic_ref<std::uint64_t>{word_ptr[0]}.store(0U, std::memory_order_relaxed);
    if (blocks > 1U) {
      std::memset(word_ptr + 1, 0, static_cast<std::size_t>(blocks - 1U) << 3);
    }
    const auto meta = producer_.value;
    producer_.value = meta.with<^^abi::ProducerMetaSpec::write_idx>(meta.get<^^abi::ProducerMetaSpec::write_idx>() - blocks)
                          .with<^^abi::ProducerMetaSpec::free_blocks>(meta.get<^^abi::ProducerMetaSpec::free_blocks>() + blocks)
                          .with<^^abi::ProducerMetaSpec::notify_after_publish>(false);
  }

  /** @brief Release-publish a completed record header at the start of the reserved region. */
  [[gnu::always_inline]] static void Publish(std::byte* record, abi::RecordHeaderWord header) noexcept {
    std::atomic_ref<std::uint64_t>{*reinterpret_cast<std::uint64_t*>(record)}.store(header.raw(), std::memory_order_release);
  }

  /** @brief Consume and clear the pending 50% watermark notification flag. */
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[nodiscard, gnu::always_inline]] bool ConsumeNotifyAfterPublish() noexcept {
    const auto meta = producer_.value;
    if (uarch::unlikely(meta.test<^^abi::ProducerMetaSpec::notify_after_publish>())) {
      producer_.value = meta.with<^^abi::ProducerMetaSpec::notify_after_publish>(false);
      return true;
    }
    return false;
  }

  /** @brief Inspect the next published record at the consumer head, transparently crossing wrap sentinels. */
  [[nodiscard, gnu::always_inline]] RecordView Front() noexcept {
    std::uint32_t c_idx = consumer_idx_.value;
    auto* word_ptr = reinterpret_cast<std::uint64_t*>(base_ + (static_cast<std::size_t>(c_idx) << 3));
    std::uint64_t raw = std::atomic_ref<std::uint64_t>{*word_ptr}.load(std::memory_order_acquire);
    if (raw == 0U) {
      return RecordView{};
    }
    auto header = abi::RecordHeaderWord::from_raw(raw);
    if (uarch::unlikely(header.get<^^abi::RecordHeaderSpec::blocks>() == abi::kWrapSentinel)) {
      std::atomic_ref<std::uint64_t>{*word_ptr}.store(0U, std::memory_order_relaxed);
      c_idx = 0U;
      consumer_idx_.value = 0U;
      read_idx_->store(0U, std::memory_order_release);
      word_ptr = reinterpret_cast<std::uint64_t*>(base_);
      raw = std::atomic_ref<std::uint64_t>{*word_ptr}.load(std::memory_order_acquire);
      if (raw == 0U) {
        return RecordView{};
      }
      header = abi::RecordHeaderWord::from_raw(raw);
    }
    return RecordView{reinterpret_cast<const std::byte*>(word_ptr), header};
  }

  /** @brief Zero consumed record blocks and advance the consumer read index with release ordering. */
  [[gnu::always_inline]] void Pop(std::uint32_t blocks) noexcept {
    if (uarch::unlikely(blocks == 0U)) {
      return;
    }
    const std::uint32_t c_idx = consumer_idx_.value;
    auto* word_ptr = reinterpret_cast<std::uint64_t*>(base_ + (static_cast<std::size_t>(c_idx) << 3));
    if (blocks > 1U) {
      std::memset(word_ptr + 1, 0, static_cast<std::size_t>(blocks - 1U) << 3);
    }
    std::atomic_ref<std::uint64_t>{word_ptr[0]}.store(0U, std::memory_order_relaxed);
    const std::uint32_t next_idx = c_idx + blocks;
    consumer_idx_.value = next_idx;
    read_idx_->store(next_idx, std::memory_order_release);
  }

  /** @brief Zero and pop the record referenced by a consumer RecordView. */
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[gnu::always_inline]] void Pop(RecordView view) noexcept { Pop(view.header.get<^^abi::RecordHeaderSpec::blocks>()); }

  /** @brief Reset producer and consumer cursors after an orphaned ring has been completely drained. */
  void ResetForReuse() noexcept {
    producer_.value = abi::ProducerMetaWord{}.with_const<^^abi::ProducerMetaSpec::free_blocks, abi::kMaxRecordBlocks>();
    consumer_idx_.value = 0U;
    read_idx_->store(0U, std::memory_order_release);
  }

  /** @brief Return the current packed producer metadata word. */
  [[nodiscard, gnu::always_inline]] abi::ProducerMetaWord ProducerMeta() const noexcept { return producer_.value; }

  /** @brief Load the consumer read index with acquire ordering. */
  [[nodiscard, gnu::always_inline]] std::uint32_t ReadIdx() const noexcept {
    return read_idx_->load(std::memory_order_acquire);
  }

  /** @brief Return the consumer-local read index cursor. */
  [[nodiscard, gnu::always_inline]] std::uint32_t ConsumerIdx() const noexcept { return consumer_idx_.value; }

  /** @brief Return the base byte pointer of the 2 MiB ring mapping. */
  [[nodiscard, gnu::always_inline]] std::byte* Base() const noexcept { return base_; }

 private:
  explicit SpscVarQueue(sys::ThpRingMapping mapping) noexcept
      : base_(mapping.Bytes().data()),
        producer_{abi::ProducerMetaWord{}.with_const<^^abi::ProducerMetaSpec::free_blocks, abi::kMaxRecordBlocks>()},
        read_idx_{0U},
        consumer_idx_{0U},
        mapping_(std::move(mapping)) {}

  std::byte* base_{nullptr};
  uarch::Isolated<abi::ProducerMetaWord> producer_;
  uarch::Isolated<std::atomic<std::uint32_t>> read_idx_;
  uarch::Isolated<std::uint32_t> consumer_idx_;
  sys::ThpRingMapping mapping_;
};

}  // namespace zerolog::core
