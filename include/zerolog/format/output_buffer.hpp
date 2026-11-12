/** @file @brief Transactional formatting into two mapped slabs under one shared unwritten-byte quota. */
#pragma once
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <zerolog/abi/record_layout.hpp>
#include <zerolog/format/header_cache.hpp>
#include <zerolog/sys/thp_mmap.hpp>
namespace zerolog::format {
inline constexpr std::uint32_t kMaxPendingBytes = 16U * 1024U * 1024U;
/** @brief A borrowed immutable slab; offset identifies the unwritten suffix. */
struct FrozenSlab {
  const char* data{};     /**< Base of the retained mapping. */
  std::uint32_t length{}; /**< Original frozen extent. */
  std::uint32_t offset{}; /**< Bytes already acknowledged by the target. */
};
static_assert(sizeof(FrozenSlab) == 16 && uarch::NoPadding<FrozenSlab> && uarch::GprPair<FrozenSlab>);
/** @brief Single-owner formatting storage. Frozen bytes remain valid across moves until completion. */
class OutputBuffer final {
 public:
  OutputBuffer(const OutputBuffer&) = delete;
  OutputBuffer& operator=(const OutputBuffer&) = delete;
  OutputBuffer(OutputBuffer&& other) noexcept
      : slabs_(std::move(other.slabs_)),
        pending_index_(other.pending_index_),
        pending_len_(std::exchange(other.pending_len_, 0)),
        frozen_(std::exchange(other.frozen_, {})),
        frozen_bytes_(std::exchange(other.frozen_bytes_, 0)),
        cache_(other.cache_) {}
  OutputBuffer& operator=(OutputBuffer&&) = delete ("replace only after all borrowed slabs are complete");
  /** @brief Reserve both arenas before accepting any records. */
  [[nodiscard]] static Result<OutputBuffer> Create() noexcept {
    auto first = sys::SlabMapping::Create();
    if (!first) return std::unexpected(first.error());
    auto second = sys::SlabMapping::Create();
    if (!second) return std::unexpected(second.error());
    return OutputBuffer{std::move(*first), std::move(*second)};
  }
  /** @brief Append a complete record, or roll back all its bytes and return the original error. */
  [[nodiscard]] ErrorCode FormatRecord(const abi::RecordPrefix& prefix, abi::DecodeCursor cursor, std::int64_t wall_ns,
                                       std::uint32_t tid, std::uint32_t slot, std::string_view thread_name = {}) noexcept {
    const auto* info = prefix.info;
    if (info == nullptr || info->decode == nullptr || (cursor.remaining != 0 && cursor.data == nullptr) ||
        (info->file_len != 0 && info->file == nullptr) || (!thread_name.empty() && thread_name.data() == nullptr))
      return ErrorCode::kInvalidArgument;
    const auto level = abi::RecordHeaderWord::from_raw(prefix.header).get<^^abi::RecordHeaderSpec::level>();
    if (level >= abi::Level::kOff) return ErrorCode::kInvalidArgument;
    const auto start = pending_len_;
    abi::OutputAppender out{PendingData(), &pending_len_, kMaxPendingBytes - frozen_bytes_};
    const auto status = [&]() noexcept {
      auto err = cache_.Append(out, wall_ns, level, tid, slot);
      if (err != ErrorCode::kSuccess) return err;
      if (!thread_name.empty()) {
        if ((err = out.Append(" [", 2)) != ErrorCode::kSuccess) return err;
        if ((err = out.Append(thread_name.data(), thread_name.size())) != ErrorCode::kSuccess) return err;
        if ((err = out.Push(']')) != ErrorCode::kSuccess) return err;
      }
      if (prefix.span_id != 0) {
        if ((err = out.Append(" [span:", 7)) != ErrorCode::kSuccess) return err;
        if ((err = FormatUint(out, prefix.span_id)) != ErrorCode::kSuccess) return err;
        if ((err = out.Push(']')) != ErrorCode::kSuccess) return err;
      }
      if ((err = out.Push(' ')) != ErrorCode::kSuccess) return err;
      if (info->file_len != 0) {
        if ((err = out.Append(info->file, info->file_len)) != ErrorCode::kSuccess) return err;
        if ((err = out.Push(':')) != ErrorCode::kSuccess) return err;
        if ((err = FormatUint(out, info->line)) != ErrorCode::kSuccess) return err;
      }
      if ((err = out.Append(": ", 2)) != ErrorCode::kSuccess) return err;
      if ((err = info->decode(info, cursor, out)) != ErrorCode::kSuccess) return err;
      return out.Push('\n');
    }();
    if (status == ErrorCode::kSuccess) return status;
    pending_len_ = start;
    return status == ErrorCode::kWouldBlock && start == 0 && frozen_bytes_ == 0 ? ErrorCode::kRecordTooLarge : status;
  }
  /** @brief Transfer the active extent to an I/O borrower in constant time. */
  [[nodiscard]] std::optional<FrozenSlab> Freeze() noexcept {
    if (frozen_bytes_ != 0 || pending_len_ == 0) return std::nullopt;
    frozen_ = FrozenSlab{PendingData(), static_cast<std::uint32_t>(pending_len_), 0};
    frozen_bytes_ = frozen_.length;
    pending_index_ ^= 1U;
    pending_len_ = 0;
    return frozen_;
  }
  /** @brief Acknowledge only a valid current borrow; short writes release credit for their written prefix. */
  [[nodiscard]] Result<std::optional<FrozenSlab>> CompleteFrozen(FrozenSlab slab, std::uint32_t written) noexcept {
    if (!Matches(slab) || written > frozen_bytes_) return std::unexpected(ErrorCode::kInvalidArgument);
    frozen_.offset += written;
    frozen_bytes_ -= written;
    if (frozen_bytes_ != 0) return std::optional{frozen_};
    frozen_ = {};
    return std::optional<FrozenSlab>{};
  }
  /** @brief Restore the unwritten suffix before newer pending bytes, preserving ordering without allocation. */
  [[nodiscard]] ErrorCode RestoreFailed(FrozenSlab slab) noexcept {
    if (!Matches(slab)) return ErrorCode::kInvalidArgument;
    if (pending_len_ > kMaxPendingBytes - frozen_bytes_) return ErrorCode::kWouldBlock;
    auto* dest = PendingData();
    std::memmove(dest + frozen_bytes_, dest, pending_len_);
    std::memcpy(dest, slab.data + slab.offset, frozen_bytes_);
    pending_len_ += frozen_bytes_;
    frozen_bytes_ = 0;
    frozen_ = {};
    return ErrorCode::kSuccess;
  }
  /** @brief Roll back a consumer budget transaction within the current pending extent. */
  [[nodiscard]] ErrorCode Rollback(std::size_t start) noexcept {
    if (start > pending_len_) return ErrorCode::kInvalidArgument;
    pending_len_ = start;
    return ErrorCode::kSuccess;
  }
  [[nodiscard]] std::size_t PendingBytes() const noexcept { return pending_len_; }
  [[nodiscard]] std::uint32_t FrozenBytes() const noexcept { return frozen_bytes_; }
  [[nodiscard]] std::size_t TotalUnflushedBytes() const noexcept { return pending_len_ + frozen_bytes_; }
  [[nodiscard]] std::span<const char> Pending() const noexcept {
    return {reinterpret_cast<const char*>(slabs_[pending_index_].Bytes().data()), pending_len_};
  }

 private:
  OutputBuffer(sys::SlabMapping first, sys::SlabMapping second) noexcept : slabs_{std::move(first), std::move(second)} {}
  [[nodiscard]] char* PendingData() noexcept { return reinterpret_cast<char*>(slabs_[pending_index_].Bytes().data()); }
  [[nodiscard]] bool Matches(FrozenSlab slab) const noexcept {
    return frozen_bytes_ != 0 && slab.data == frozen_.data && slab.length == frozen_.length && slab.offset == frozen_.offset;
  }
  std::array<sys::SlabMapping, 2> slabs_; /**< Each mapping retains its complete owner. */
  std::uint32_t pending_index_{};
  std::size_t pending_len_{};
  FrozenSlab frozen_{};
  std::uint32_t frozen_bytes_{};
  HeaderCache cache_;
};
}  // namespace zerolog::format
