/** @file @brief Size-based log rotation with a stable path table and a synchronous writer. */
#pragma once

#include <fcntl.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <uarch/uarch.hpp>
#include <uring/ops/fs.hpp>
#include <uring/sys/syscall.hpp>
#include <uring/sys/unique_fd.hpp>
#include <zerolog/error/zerolog_error.hpp>

namespace zerolog::sink {

/**
 * @brief Direct `write` and `openat` used by the release rotation type.
 * A test translation unit instantiates `BasicRotation` on its own type; this one has no hook field.
 */
struct PosixIo {
  [[nodiscard]] static std::int64_t Write(std::int32_t fd, const std::byte* data, std::uint32_t length) noexcept {
    const auto descriptor = static_cast<std::uint64_t>(static_cast<std::int64_t>(fd));
    return uring::sys::Syscall6(kSysWrite, descriptor, uring::sys::Address(data), length);
  }

  [[nodiscard]] static std::int64_t Open(const char* path, std::uint32_t flags, std::uint32_t mode) noexcept {
    const auto directory = static_cast<std::uint64_t>(static_cast<std::int64_t>(AT_FDCWD));
    return uring::sys::Syscall6(kSysOpenat, directory, uring::sys::Address(path), flags, mode);
  }

  /** @brief `unlinkat`. `flags` is the kernel `unlinkat` flag word (`AT_REMOVEDIR` or zero). */
  [[nodiscard]] static std::int64_t UnlinkAt(int dirfd, const char* pathname, int flags) noexcept {
    const auto directory = static_cast<std::uint64_t>(static_cast<std::int64_t>(dirfd));
    return uring::sys::Syscall6(kSysUnlinkat, directory, uring::sys::Address(pathname),
                                static_cast<std::uint64_t>(static_cast<std::uint32_t>(flags)));
  }

  /**
   * @brief `renameat` when `flags == 0`, otherwise `renameat2`.
   * A non-zero flag word must reach the kernel; `renameat` has nowhere to put it.
   */
  [[nodiscard]] static std::int64_t RenameAt(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
                                             unsigned int flags) noexcept {
    const auto old_dir = static_cast<std::uint64_t>(static_cast<std::int64_t>(olddirfd));
    const auto new_dir = static_cast<std::uint64_t>(static_cast<std::int64_t>(newdirfd));
    if (flags == 0) {
      return uring::sys::Syscall6(kSysRenameat, old_dir, uring::sys::Address(oldpath), new_dir, uring::sys::Address(newpath));
    }
    return uring::sys::Syscall6(kSysRenameat2, old_dir, uring::sys::Address(oldpath), new_dir, uring::sys::Address(newpath),
                                flags);
  }

  static constexpr std::int64_t kSysWrite = 1;
  static constexpr std::int64_t kSysOpenat = 257;
  static constexpr std::int64_t kSysUnlinkat = 263;
  static constexpr std::int64_t kSysRenameat = 264;
  static constexpr std::int64_t kSysRenameat2 = 316;
};

/** @brief Active-file size cap and how many numbered archives to retain. */
struct RotationPolicy {
  std::uint64_t max_bytes{64 * 1024 * 1024};  ///< Rotate after the active file reaches this size.
  std::uint8_t max_files{5};                  ///< Highest archive index, in `1..15`.
  std::uint8_t reserved[7]{};                 ///< Keeps the policy a 16-byte register pair.
};
static_assert(sizeof(RotationPolicy) == 16 && uarch::NoPadding<RotationPolicy> && uarch::GprPair<RotationPolicy>);

/** @brief Bytes a synchronous write committed, even when a later attempt fails. */
struct WriteProgress {
  std::uint32_t written{};               ///< Prefix accepted by the kernel for this call.
  ErrorCode error{ErrorCode::kSuccess};  ///< `kSuccess` when the whole span was accepted.
  std::uint8_t reserved[3]{};            ///< Keeps the result an 8-byte register value.
};
static_assert(sizeof(WriteProgress) == 8 && uarch::NoPadding<WriteProgress> && uarch::RegisterPassable<WriteProgress>);

/**
 * @brief Owns one append-only log and its `base` .. `base.15` name table.
 *
 * SQE path pointers alias `paths_`, so the object cannot move for the lifetime of those submissions.
 * One rotation attempt unlinks `base.{max_files}` once, then renames downward from `next_rename_slot_`.
 * `ENOENT` on an archive slot advances the cursor. `ENOENT` on `base` means that name is already gone,
 * so the attempt opens a replacement instead of failing. Any other rename error stays on that slot.
 * After the chain finishes, `reopen_pending_` retries only `openat`.
 * `WriteSync` does not rotate; the caller checks `NeedsRotation` and calls `RotateSync`.
 */
template <class Io = PosixIo>
class BasicRotation {
 public:
  static constexpr std::size_t kPathSlots = 16;
  static constexpr std::size_t kPathCap = 4096;

  BasicRotation() noexcept = default;
  BasicRotation(const BasicRotation&) = delete ("kernel holds path pointers into Rotation");
  BasicRotation& operator=(const BasicRotation&) = delete ("kernel holds path pointers into Rotation");
  BasicRotation(BasicRotation&&) = delete ("kernel holds path pointers into Rotation");
  BasicRotation& operator=(BasicRotation&&) = delete ("kernel holds path pointers into Rotation");

  /**
   * @brief Create or append `base_path` and precompute `base`, `base.1` .. `base.15`.
   * @param base_path NUL-free path. The active file is opened `O_APPEND` without truncation.
   * Already open, or `max_files` outside `1..15`, is `kInvalidArgument`. A failed `openat` is `kRotateFailed`.
   */
  [[nodiscard]] Result<void> Open(std::string_view base_path, RotationPolicy policy) noexcept {
    if (uarch::unlikely(current_fd_.valid())) return std::unexpected(ErrorCode::kInvalidArgument);
    if (uarch::unlikely(policy.max_files == 0 || policy.max_files > 15)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (base_path.empty() || base_path.size() + 4 >= kPathCap || base_path.find('\0') != std::string_view::npos) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    std::array<char, kPathCap> primary_path{};
    std::memcpy(primary_path.data(), base_path.data(), base_path.size());
    primary_path[base_path.size()] = '\0';
    const auto opened = Io::Open(primary_path.data(), kAppendFlags, kFileMode);
    if (uarch::unlikely(opened < 0)) return std::unexpected(ErrorCode::kRotateFailed);
    auto adopted = uring::sys::UniqueFd::Adopt(static_cast<std::int32_t>(opened));
    if (uarch::unlikely(!adopted)) {
      CloseRaw(static_cast<std::int32_t>(opened));
      return std::unexpected(ErrorCode::kRotateFailed);
    }
    StorePaths(paths_, base_path);
    policy_ = policy;
    retired_fd_ = uring::sys::UniqueFd{};
    current_fd_ = std::move(*adopted);
    reopen_pending_ = 0;
    oldest_unlinked_ = 0;
    next_rename_slot_ = 0;
    written_bytes_ = CurrentSize();
    return {};
  }

  /**
   * @brief Adopt a duplicated `O_CLOEXEC` copy of an open stream descriptor (`STDERR_FILENO`, pipe, or file) without rotation.
   */
  [[nodiscard]] Result<void> OpenFd(std::int32_t fd) noexcept {
    if (uarch::unlikely(current_fd_.valid() || fd < 0)) return std::unexpected(ErrorCode::kInvalidArgument);
    constexpr std::uint64_t f_dupfd_cloexec = 1030;
    const auto duped = uring::sys::Syscall6(uring::sys::kSysFcntl, DirArg(fd), f_dupfd_cloexec, 0);
    if (uarch::unlikely(duped < 0)) return std::unexpected(ErrorCode::kRotateFailed);
    auto adopted = uring::sys::UniqueFd::Adopt(static_cast<std::int32_t>(duped));
    if (uarch::unlikely(!adopted)) {
      CloseRaw(static_cast<std::int32_t>(duped));
      return std::unexpected(ErrorCode::kRotateFailed);
    }
    paths_[0][0] = '\0';
    policy_ = RotationPolicy{.max_bytes = ~0ULL, .max_files = 1};
    retired_fd_ = uring::sys::UniqueFd{};
    current_fd_ = std::move(*adopted);
    reopen_pending_ = 0;
    oldest_unlinked_ = 0;
    next_rename_slot_ = 0;
    written_bytes_ = 0;
    return {};
  }

  /** @brief Append `length` bytes at the kernel file position (`offset == ~0ULL`) on the `O_APPEND` fd. */
  [[nodiscard, gnu::always_inline]] uring::ops::Write PrepareWrite(const std::byte* data, std::uint32_t length) const noexcept {
    return uring::ops::PrepareWrite(current_fd_.get(), uring::ops::UserAddress{data}, length, ~0ULL, 0);
  }

  /** @brief Append `length` bytes from a character buffer at the kernel file position (`offset == ~0ULL`). */
  [[nodiscard, gnu::always_inline]] uring::ops::Write PrepareWrite(const char* data, std::uint32_t length) const noexcept {
    return PrepareWrite(reinterpret_cast<const std::byte*>(data), length);
  }

  /** @brief Append the remaining `slab.length - slab.offset` suffix of a `FrozenSlab` at `~0ULL`. */
  template <class Slab>
    requires requires(const Slab& s) {
      s.data;
      s.offset;
      s.length;
    }
  [[nodiscard, gnu::always_inline]] uring::ops::Write PrepareWrite(const Slab& slab) const noexcept {
    const std::uint32_t rem = slab.length >= slab.offset ? (slab.length - slab.offset) : 0U;
    return PrepareWrite(slab.data + slab.offset, rem);
  }

  /** @brief Unlink `base.{max_files}` before the archive chain shifts into that slot. */
  [[nodiscard, gnu::always_inline]] uring::ops::Unlinkat PrepareUnlinkOldest() const noexcept {
    return uring::ops::PrepareUnlinkat(AT_FDCWD, uring::ops::UserAddress{paths_[policy_.max_files].data()}, 0);
  }

  /**
   * @brief Rename `paths_[step]` onto `paths_[step + 1]`.
   * Callers walk `step` from `max_files - 1` down to `0` so newer archives move first.
   */
  [[nodiscard, gnu::always_inline]] uring::ops::Renameat PrepareRenameStep(std::uint8_t step) const noexcept {
    const auto source = step < kPathSlots - 1 ? step : static_cast<std::uint8_t>(kPathSlots - 2);
    return uring::ops::PrepareRenameat(AT_FDCWD, uring::ops::UserAddress{paths_[source].data()}, AT_FDCWD,
                                       uring::ops::UserAddress{paths_[static_cast<std::size_t>(source) + 1].data()}, 0);
  }

  /** @brief Create a truncated `base` after the previous inode has been renamed to `base.1`. */
  [[nodiscard, gnu::always_inline]] uring::ops::Openat PrepareOpenNew() const noexcept {
    return uring::ops::PrepareOpenat(AT_FDCWD, uring::ops::UserAddress{paths_[0].data()}, kRotateFlags, kFileMode);
  }

  /**
   * @brief Publish a newly opened `base` and retire the descriptor that now refers to `base.1`.
   * A negative fd, or an fd that cannot be owned, leaves the live descriptor and the resume cursor unchanged.
   */
  void AdoptNewFd(std::int32_t new_fd) noexcept {
    if (uarch::unlikely(new_fd < 0)) return;
    auto adopted = uring::sys::UniqueFd::Adopt(new_fd);
    if (uarch::unlikely(!adopted)) {
      CloseRaw(new_fd);
      return;
    }
    retired_fd_ = std::move(current_fd_);
    current_fd_ = std::move(*adopted);
    written_bytes_ = 0;
    reopen_pending_ = 0;
    oldest_unlinked_ = 0;
    next_rename_slot_ = 0;
  }

  /**
   * @brief Append `data`, retrying `EINTR`.
   * A positive prefix stays committed when a later `write` fails. A `0` return on a non-empty span is
   * `kShortWrite`. This call does not rotate, including when `NeedsRotation` becomes true.
   */
  [[nodiscard]] WriteProgress WriteSync(std::span<const std::byte> data) noexcept {
    if (uarch::unlikely(!current_fd_.valid())) return WriteProgress{.error = ErrorCode::kInvalidArgument};
    if (uarch::unlikely(data.size() > 0xffffffffU)) return WriteProgress{.error = ErrorCode::kInvalidArgument};
    const auto length = static_cast<std::uint32_t>(data.size());
    std::uint32_t committed = 0;
    while (committed < length) {
      const auto result = Io::Write(current_fd_.get(), data.data() + committed, length - committed);
      if (uarch::unlikely(result < 0)) {
        if (result == -static_cast<std::int64_t>(EINTR)) continue;
        written_bytes_ += committed;
        return WriteProgress{.written = committed, .error = ErrorCode::kWriteFailed};
      }
      if (uarch::unlikely(result == 0)) {
        written_bytes_ += committed;
        return WriteProgress{.written = committed, .error = ErrorCode::kShortWrite};
      }
      const auto step = static_cast<std::uint32_t>(result);
      // A count past the request is not a finished span, so the prefix must not be treated as complete.
      if (uarch::unlikely(step > length - committed)) {
        written_bytes_ += committed;
        return WriteProgress{.written = committed, .error = ErrorCode::kWriteFailed};
      }
      committed += step;
    }
    written_bytes_ += committed;
    return WriteProgress{.written = committed};
  }

  /** @brief Append a character span synchronously, retrying `EINTR`. */
  [[nodiscard]] WriteProgress WriteSync(std::span<const char> data) noexcept {
    return WriteSync(std::span<const std::byte>{reinterpret_cast<const std::byte*>(data.data()), data.size()});
  }

  /** @brief Synchronously write the remaining suffix of a `FrozenSlab`, advancing `slab.offset` by committed bytes. */
  template <class Slab>
    requires requires(Slab& s) {
      s.data;
      s.offset;
      s.length;
    }
  [[nodiscard]] WriteProgress WriteSlabSync(Slab& slab) noexcept {
    const std::uint32_t rem = slab.length >= slab.offset ? (slab.length - slab.offset) : 0U;
    const auto progress = WriteSync(std::span<const char>{slab.data + slab.offset, rem});
    slab.offset += progress.written;
    return progress;
  }

  /**
   * @brief Shift archives and open a new `base`, resuming a cursor left by an earlier failure.
   * The oldest unlink runs once per attempt. A later retry continues at `next_rename_slot_`, or only
   * reopens `base` once every rename has been accepted. `openat` failure keeps the previous descriptor,
   * which still writes the inode now named `base.1` when that rename already happened.
   */
  [[nodiscard]] Result<void> RotateSync() noexcept {
    if (uarch::unlikely(!current_fd_.valid() && reopen_pending_ == 0 && oldest_unlinked_ == 0)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (reopen_pending_ == 0) {
      if (oldest_unlinked_ == 0) {
        const auto removed = Io::UnlinkAt(AT_FDCWD, paths_[policy_.max_files].data(), 0);
        if (uarch::unlikely(removed < 0 && removed != -static_cast<std::int64_t>(ENOENT))) {
          return std::unexpected(ErrorCode::kRotateFailed);
        }
        oldest_unlinked_ = 1;
        next_rename_slot_ = static_cast<std::uint8_t>(policy_.max_files - 1);
      }
      while (reopen_pending_ == 0) {
        const std::uint8_t slot = next_rename_slot_;
        const auto renamed =
            Io::RenameAt(AT_FDCWD, paths_[slot].data(), AT_FDCWD, paths_[static_cast<std::size_t>(slot) + 1].data(), 0);
        if (uarch::unlikely(renamed < 0 && renamed != -static_cast<std::int64_t>(ENOENT))) {
          return std::unexpected(ErrorCode::kRotateFailed);
        }
        if (slot == 0) {
          reopen_pending_ = 1;
          break;
        }
        next_rename_slot_ = static_cast<std::uint8_t>(slot - 1);
      }
    }
    const auto opened = Io::Open(paths_[0].data(), kRotateFlags, kFileMode);
    if (uarch::unlikely(opened < 0)) return std::unexpected(ErrorCode::kRotateFailed);
    AdoptNewFd(static_cast<std::int32_t>(opened));
    if (uarch::unlikely(reopen_pending_ != 0)) return std::unexpected(ErrorCode::kRotateFailed);
    return {};
  }

  [[nodiscard]] bool NeedsRotation() const noexcept {
    return paths_[0][0] != '\0' && written_bytes_ != 0 && written_bytes_ >= policy_.max_bytes;
  }
  [[nodiscard]] bool IsOpen() const noexcept { return current_fd_.valid(); }
  [[nodiscard]] std::int32_t Fd() const noexcept { return current_fd_.get(); }
  [[nodiscard]] RotationPolicy Policy() const noexcept { return policy_; }
  [[nodiscard]] std::uint8_t MaxFiles() const noexcept { return policy_.max_files; }
  [[nodiscard]] bool ReopenPending() const noexcept { return reopen_pending_ != 0; }
  [[nodiscard]] bool OldestUnlinked() const noexcept { return oldest_unlinked_ != 0; }
  /**
   * @brief Record bytes accepted by an asynchronous `WRITE` completion on the active descriptor.
   */
  [[nodiscard]] Result<void> OnWriteDone(std::uint32_t written) noexcept {
    if (uarch::unlikely(!current_fd_.valid())) return std::unexpected(ErrorCode::kInvalidArgument);
    written_bytes_ += written;
    return {};
  }
  /**
   * @brief Next rename source.
   * Before an attempt starts this is `max_files - 1` and does not publish the cursor. `max_files == 0` has no slot,
   * so the value stays `0` instead of wrapping.
   */
  [[nodiscard]] std::uint8_t NextRenameSlot() const noexcept {
    if (oldest_unlinked_ == 0 && reopen_pending_ == 0) {
      if (policy_.max_files == 0) return 0;
      return static_cast<std::uint8_t>(policy_.max_files - 1);
    }
    return next_rename_slot_;
  }
  [[nodiscard]] std::uint64_t WrittenBytes() const noexcept { return written_bytes_; }
  [[nodiscard]] const char* PathAt(std::uint8_t index) const noexcept { return paths_[index < kPathSlots ? index : 0].data(); }

  /**
   * @brief Record an async `unlinkat` of `base.{max_files}`.
   * `0` and `-ENOENT` publish the cursor at `max_files - 1`. Any other result leaves the cursor unpublished.
   */
  [[nodiscard]] Result<void> OnUnlinkOldestDone(std::int32_t cqe_res) noexcept {
    if (uarch::unlikely(!current_fd_.valid() || policy_.max_files == 0 || policy_.max_files > 15)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (uarch::unlikely(oldest_unlinked_ != 0 || reopen_pending_ != 0)) return std::unexpected(ErrorCode::kInvalidArgument);
    if (cqe_res != 0 && cqe_res != -static_cast<std::int32_t>(ENOENT)) return std::unexpected(ErrorCode::kRotateFailed);
    oldest_unlinked_ = 1;
    next_rename_slot_ = static_cast<std::uint8_t>(policy_.max_files - 1);
    return {};
  }

  /**
   * @brief Record an async rename of `paths_[slot]` onto the next generation.
   * The slot must be the current cursor. `0` and `-ENOENT` advance it; slot `0` finishes the chain.
   * Any other errno leaves `next_rename_slot_` on `slot`.
   */
  [[nodiscard]] Result<void> OnRenameStepDone(std::uint8_t slot, std::int32_t cqe_res) noexcept {
    if (uarch::unlikely(!current_fd_.valid() || oldest_unlinked_ == 0 || reopen_pending_ != 0 || slot != next_rename_slot_ ||
                        slot >= policy_.max_files)) {
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    if (cqe_res != 0 && cqe_res != -static_cast<std::int32_t>(ENOENT)) return std::unexpected(ErrorCode::kRotateFailed);
    if (slot == 0) {
      reopen_pending_ = 1;
      return {};
    }
    next_rename_slot_ = static_cast<std::uint8_t>(slot - 1);
    return {};
  }

  /**
   * @brief Adopt the replacement `base` descriptor and hand the previous descriptor to the caller.
   * A negative completion leaves `reopen_pending_` set. The owning type is `UniqueFd` (this uring has no `OwnedFd`).
   * The caller closes the returned descriptor; it is not retained in `retired_fd_`.
   */
  [[nodiscard]] Result<uring::sys::UniqueFd> OnOpenNewDone(std::int32_t cqe_res) noexcept {
    if (cqe_res < 0) return std::unexpected(ErrorCode::kRotateFailed);
    if (uarch::unlikely(reopen_pending_ == 0)) {
      CloseRaw(cqe_res);
      return std::unexpected(ErrorCode::kInvalidArgument);
    }
    uring::sys::UniqueFd previous = std::move(current_fd_);
    AdoptNewFd(cqe_res);
    if (uarch::unlikely(reopen_pending_ != 0)) {
      current_fd_ = std::move(previous);
      return std::unexpected(ErrorCode::kRotateFailed);
    }
    return previous;
  }

 private:
  using PathSlot = std::array<char, kPathCap>;
  using PathTable = std::array<PathSlot, kPathSlots>;

  static constexpr std::uint32_t kAppendFlags = static_cast<std::uint32_t>(O_WRONLY) | static_cast<std::uint32_t>(O_CREAT) |
                                                static_cast<std::uint32_t>(O_APPEND) | static_cast<std::uint32_t>(O_CLOEXEC);
  static constexpr std::uint32_t kRotateFlags = kAppendFlags | static_cast<std::uint32_t>(O_TRUNC);
  static constexpr std::uint32_t kFileMode = 0644;
  static constexpr std::uint64_t kAtEmptyPath = 0x1000;
  static constexpr std::uint64_t kStatxSize = 0x200;
  static constexpr std::size_t kStatxSizeOffset = 40;

  static void StorePaths(PathTable& table, std::string_view base_path) noexcept {
    for (std::uint8_t index = 0; index < kPathSlots; ++index) {
      auto& slot = table[index];
      std::memcpy(slot.data(), base_path.data(), base_path.size());
      std::size_t length = base_path.size();
      if (index != 0) {
        slot[length++] = '.';
        if (index >= 10) {
          slot[length++] = '1';
          slot[length++] = static_cast<char>('0' + static_cast<int>(index - 10));
        } else {
          slot[length++] = static_cast<char>('0' + static_cast<int>(index));
        }
      }
      slot[length] = '\0';
    }
  }

  [[nodiscard]] static std::uint64_t DirArg(std::int32_t fd) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(fd));
  }

  static void CloseRaw(std::int32_t fd) noexcept {
    if (fd < 0) return;
    // close releases the descriptor even when it reports EINTR.
    [[maybe_unused]] const auto closed = uring::sys::Syscall6(uring::sys::kSysClose, DirArg(fd));
  }

  [[nodiscard]] std::uint64_t CurrentSize() const noexcept {
    alignas(8) std::array<std::byte, 256> statx{};
    const auto status = uring::sys::Syscall6(uring::sys::kSysStatx, DirArg(current_fd_.get()), uring::sys::Address(""),
                                             kAtEmptyPath, kStatxSize, uring::sys::Address(statx.data()));
    if (uarch::unlikely(status < 0)) return 0;
    std::uint64_t size = 0;
    std::memcpy(&size, statx.data() + kStatxSizeOffset, sizeof(size));
    return size;
  }

  PathTable paths_{};                  ///< NUL-terminated `base`, `base.1` .. `base.15`. SQEs alias these buffers.
  uring::sys::UniqueFd current_fd_{};  ///< Descriptor currently accepting appends.
  uring::sys::UniqueFd retired_fd_{};  ///< Previous generation, closed when the next adopt replaces it.
  RotationPolicy policy_{};            ///< Size cap and archive depth captured at `Open`.
  std::uint64_t written_bytes_{};      ///< Bytes accepted on the active inode since it was opened.
  std::uint8_t reopen_pending_{};      ///< Rename chain finished; only `openat` of `base` remains.
  std::uint8_t oldest_unlinked_{};     ///< `base.{max_files}` was already removed for this attempt.
  std::uint8_t next_rename_slot_{};    ///< Source index of the next `paths_[slot] -> paths_[slot + 1]`.
};

/** @brief Release rotation type. Its `write` and `openat` calls are direct syscalls. */
using Rotation = BasicRotation<PosixIo>;

}  // namespace zerolog::sink

namespace zerolog {
using sink::Rotation;
using sink::RotationPolicy;
using sink::WriteProgress;
}  // namespace zerolog
