#include "zerolog/sink/rotation.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/resource.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <uring/abi/uapi_io_uring.hpp>
#include <uring/sys/syscall.hpp>

namespace {

int g_failures = 0;
int g_write_phase = 0;
int g_rename_eperm_slot = -1;

enum class Fault : std::uint8_t { kNone, kPartial, kEintr, kZero };

Fault g_fault = Fault::kNone;

void Check(bool condition, const char* expression, int line) noexcept {
  if (condition) return;
  std::fprintf(stderr, "FAIL %s:%d %s\n", "rotation_test.cc", line, expression);
  ++g_failures;
}

#define CHECK(condition)                  \
  do {                                    \
    if (!(condition)) {                   \
      Check(false, #condition, __LINE__); \
      return;                             \
    }                                     \
  } while (0)

struct TempDir {
  std::array<char, 64> path{};
  explicit TempDir() noexcept {
    std::snprintf(path.data(), path.size(), "/tmp/zl-rot-XXXXXX");
    if (::mkdtemp(path.data()) == nullptr) path[0] = '\0';
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() noexcept {
    if (path[0] == '\0') return;
    for (int index = 0; index <= 15; ++index) {
      char file[128]{};
      if (index == 0)
        std::snprintf(file, sizeof(file), "%s/base", path.data());
      else
        std::snprintf(file, sizeof(file), "%s/base.%d", path.data(), index);
      ::unlink(file);
    }
    ::rmdir(path.data());
  }
  [[nodiscard]] bool Join(char* out, std::size_t out_size, const char* name) const noexcept {
    if (path[0] == '\0') return false;
    const auto wrote = std::snprintf(out, out_size, "%s/%s", path.data(), name);
    return wrote > 0 && static_cast<std::size_t>(wrote) < out_size;
  }
};

/** Lowers RLIMIT_NOFILE to the first free descriptor so the next openat returns EMFILE. */
class NoFileCeiling {
 public:
  NoFileCeiling() noexcept = default;
  NoFileCeiling(const NoFileCeiling&) = delete;
  NoFileCeiling& operator=(const NoFileCeiling&) = delete;
  ~NoFileCeiling() { Release(); }

  [[nodiscard]] bool Engage() noexcept {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) return false;
    const int self = ::dirfd(directory);
    bool seen[4096]{};
    int highest = 0;
    while (const dirent* entry = ::readdir(directory)) {
      int value = 0;
      const char* cursor = entry->d_name;
      if (*cursor < '0' || *cursor > '9') continue;
      while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10 + (*cursor - '0');
        ++cursor;
      }
      if (value == self || value < 0 || value >= static_cast<int>(sizeof(seen))) continue;
      seen[value] = true;
      if (value > highest) highest = value;
    }
    ::closedir(directory);
    int minimum_free = 0;
    while (minimum_free <= highest && seen[minimum_free]) ++minimum_free;
    if (::getrlimit(RLIMIT_NOFILE, &saved_) != 0) return false;
    rlimit tightened = saved_;
    if (static_cast<rlim_t>(minimum_free) > tightened.rlim_max) return false;
    tightened.rlim_cur = static_cast<rlim_t>(minimum_free);
    if (::setrlimit(RLIMIT_NOFILE, &tightened) != 0) return false;
    engaged_ = true;
    return true;
  }

  void Release() noexcept {
    if (!engaged_) return;
    ::setrlimit(RLIMIT_NOFILE, &saved_);
    engaged_ = false;
  }

 private:
  rlimit saved_{};
  bool engaged_{false};
};

/** Holds FS_IMMUTABLE_FL so rename of this inode fails with EPERM until Clear. */
class ImmutableFile {
 public:
  explicit ImmutableFile(const char* path) noexcept {
    fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
      arm_errno_ = errno;
      return;
    }
    if (!Set(true)) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  ImmutableFile(const ImmutableFile&) = delete;
  ImmutableFile& operator=(const ImmutableFile&) = delete;
  ~ImmutableFile() { Clear(); }
  [[nodiscard]] bool Active() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int ArmErrno() const noexcept { return arm_errno_; }
  void Clear() noexcept {
    if (fd_ < 0) return;
    [[maybe_unused]] const auto cleared = Set(false);
    ::close(fd_);
    fd_ = -1;
  }

 private:
  [[nodiscard]] bool Set(bool enable) noexcept {
    long flags = 0;
    const auto read = FlagCtl(static_cast<std::uint64_t>(FS_IOC_GETFLAGS), &flags);
    if (read < 0) {
      arm_errno_ = static_cast<int>(-read);
      return false;
    }
    if (enable)
      flags |= FS_IMMUTABLE_FL;
    else
      flags &= ~static_cast<long>(FS_IMMUTABLE_FL);
    const auto wrote = FlagCtl(static_cast<std::uint64_t>(FS_IOC_SETFLAGS), &flags);
    if (wrote < 0) {
      arm_errno_ = static_cast<int>(-wrote);
      return false;
    }
    return true;
  }

  // musl declares ioctl with an int request, which cannot hold the 8-byte `_IOR('f', ...)` encoding.
  [[nodiscard]] std::int64_t FlagCtl(std::uint64_t request, long* flags) const noexcept {
    return uring::sys::Syscall6(16, static_cast<std::uint64_t>(static_cast<std::int64_t>(fd_)), request,
                                uring::sys::Address(flags));
  }
  int fd_{-1};
  int arm_errno_{0};
};

[[nodiscard]] int SourceSlot(const char* path) noexcept;

struct FaultIo {
  [[nodiscard]] static std::int64_t Write(std::int32_t fd, const std::byte* data, std::uint32_t length) noexcept {
    if (g_fault == Fault::kPartial) {
      if (g_write_phase == 0) {
        ++g_write_phase;
        return -EINTR;
      }
      if (g_write_phase == 1) {
        ++g_write_phase;
        if (length == 0) return 0;
        return zerolog::sink::PosixIo::Write(fd, data, 1);
      }
      ++g_write_phase;
      return -EIO;
    }
    if (g_fault == Fault::kEintr) {
      if (g_write_phase == 0) {
        ++g_write_phase;
        return -EINTR;
      }
      return zerolog::sink::PosixIo::Write(fd, data, length);
    }
    if (g_fault == Fault::kZero) return 0;
    return zerolog::sink::PosixIo::Write(fd, data, length);
  }

  [[nodiscard]] static std::int64_t Open(const char* path, std::uint32_t flags, std::uint32_t mode) noexcept {
    return zerolog::sink::PosixIo::Open(path, flags, mode);
  }

  [[nodiscard]] static std::int64_t UnlinkAt(int dirfd, const char* pathname, int flags) noexcept {
    return zerolog::sink::PosixIo::UnlinkAt(dirfd, pathname, flags);
  }

  [[nodiscard]] static std::int64_t RenameAt(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
                                             unsigned int flags) noexcept {
    if (g_rename_eperm_slot >= 0 && SourceSlot(oldpath) == g_rename_eperm_slot) return -EPERM;
    return zerolog::sink::PosixIo::RenameAt(olddirfd, oldpath, newdirfd, newpath, flags);
  }
};

[[nodiscard]] int SourceSlot(const char* path) noexcept {
  if (path == nullptr) return -1;
  const char* slash = std::strrchr(path, '/');
  const char* name = slash == nullptr ? path : slash + 1;
  const char* dot = std::strrchr(name, '.');
  if (dot == nullptr) return 0;
  int slot = 0;
  for (const char* cursor = dot + 1; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return -1;
    slot = slot * 10 + (*cursor - '0');
  }
  return slot;
}

using FaultRotation = zerolog::sink::BasicRotation<FaultIo>;

[[nodiscard]] std::span<const std::byte> Bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] bool ReadEquals(const char* path, std::string_view expect) noexcept {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::array<char, 64> buffer{};
  const auto count = ::read(fd, buffer.data(), buffer.size());
  ::close(fd);
  if (count < 0 || static_cast<std::size_t>(count) != expect.size()) return false;
  return expect.empty() || std::memcmp(buffer.data(), expect.data(), expect.size()) == 0;
}

[[nodiscard]] bool Missing(const char* path) noexcept {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ::close(fd);
    return false;
  }
  return errno == ENOENT;
}

[[nodiscard]] bool WriteFile(const char* path, std::string_view text) noexcept {
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const auto wrote = ::write(fd, text.data(), text.size());
  ::close(fd);
  return wrote == static_cast<ssize_t>(text.size());
}

void GenerationsKeepExactBytesTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  char base2[128]{};
  char base3[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  CHECK(dir.Join(base2, sizeof(base2), "base.2"));
  CHECK(dir.Join(base3, sizeof(base3), "base.3"));
  zerolog::Rotation rotation;
  const auto opened = rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 4, .max_files = 3});
  CHECK(opened.has_value());
  const auto first = rotation.WriteSync(Bytes("AAAA"));
  CHECK(first.error == zerolog::ErrorCode::kSuccess);
  CHECK(first.written == 4);
  CHECK(rotation.NeedsRotation());
  CHECK(rotation.RotateSync().has_value());
  CHECK(ReadEquals(base, ""));
  CHECK(ReadEquals(base1, "AAAA"));
  CHECK(Missing(base2));
  const auto second = rotation.WriteSync(Bytes("BBBB"));
  CHECK(second.error == zerolog::ErrorCode::kSuccess);
  CHECK(rotation.RotateSync().has_value());
  CHECK(ReadEquals(base1, "BBBB"));
  CHECK(ReadEquals(base2, "AAAA"));
  const auto third = rotation.WriteSync(Bytes("CCCC"));
  CHECK(third.error == zerolog::ErrorCode::kSuccess);
  CHECK(rotation.RotateSync().has_value());
  CHECK(ReadEquals(base1, "CCCC"));
  CHECK(ReadEquals(base2, "BBBB"));
  CHECK(ReadEquals(base3, "AAAA"));
  const auto tail = rotation.WriteSync(Bytes("DD"));
  CHECK(tail.error == zerolog::ErrorCode::kSuccess);
  CHECK(tail.written == 2);
  CHECK(!rotation.NeedsRotation());
  CHECK(ReadEquals(base, "DD"));
  CHECK(ReadEquals(base1, "CCCC"));
  CHECK(ReadEquals(base2, "BBBB"));
  CHECK(ReadEquals(base3, "AAAA"));
  CHECK(!rotation.ReopenPending());
}

void EintrPartialKeepsPrefixTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  FaultRotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 1, .max_files = 3}).has_value());
  g_write_phase = 0;
  g_fault = Fault::kPartial;
  const auto progress = rotation.WriteSync(Bytes("ABCD"));
  g_fault = Fault::kNone;
  CHECK(progress.written == 1);
  CHECK(progress.error == zerolog::ErrorCode::kWriteFailed);
  CHECK(g_write_phase == 3);
  CHECK(ReadEquals(base, "A"));
  CHECK(Missing(base1));
  CHECK(rotation.WrittenBytes() == 1);
  CHECK(rotation.NeedsRotation());
}

void EintrThenFullCommitTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  FaultRotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 4, .max_files = 2}).has_value());
  g_write_phase = 0;
  g_fault = Fault::kEintr;
  const auto progress = rotation.WriteSync(Bytes("WXYZ"));
  g_fault = Fault::kNone;
  CHECK(progress.written == 4);
  CHECK(progress.error == zerolog::ErrorCode::kSuccess);
  CHECK(ReadEquals(base, "WXYZ"));
  CHECK(Missing(base1));
  CHECK(rotation.NeedsRotation());
  CHECK(rotation.RotateSync().has_value());
  CHECK(ReadEquals(base, ""));
  CHECK(ReadEquals(base1, "WXYZ"));
}

void ZeroReturnReportsShortTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  FaultRotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 1, .max_files = 2}).has_value());
  g_fault = Fault::kZero;
  const auto progress = rotation.WriteSync(Bytes("ZZ"));
  g_fault = Fault::kNone;
  CHECK(progress.written == 0);
  CHECK(progress.error == zerolog::ErrorCode::kShortWrite);
  CHECK(ReadEquals(base, ""));
  CHECK(Missing(base1));
  CHECK(rotation.WrittenBytes() == 0);
  CHECK(!rotation.NeedsRotation());
}

void BackoffWriteStaysSuccessTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  char base2[128]{};
  char base3[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  CHECK(dir.Join(base2, sizeof(base2), "base.2"));
  CHECK(dir.Join(base3, sizeof(base3), "base.3"));
  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 4, .max_files = 3}).has_value());
  CHECK(rotation.WriteSync(Bytes("AAAA")).error == zerolog::ErrorCode::kSuccess);
  CHECK(rotation.RotateSync().has_value());
  CHECK(rotation.WriteSync(Bytes("BBBB")).error == zerolog::ErrorCode::kSuccess);
  const auto live_fd = rotation.Fd();
  NoFileCeiling ceiling;
  CHECK(ceiling.Engage());
  const auto failed = rotation.RotateSync();
  CHECK(!failed.has_value());
  CHECK(failed.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(rotation.ReopenPending());
  CHECK(rotation.Fd() == live_fd);
  const auto during = rotation.WriteSync(Bytes("XY"));
  CHECK(during.written == 2);
  CHECK(during.error == zerolog::ErrorCode::kSuccess);
  CHECK(rotation.Fd() == live_fd);
  CHECK(rotation.ReopenPending());
  const auto still_blocked = rotation.RotateSync();
  CHECK(!still_blocked.has_value());
  CHECK(rotation.Fd() == live_fd);
  ceiling.Release();
  CHECK(Missing(base));
  CHECK(ReadEquals(base1, "BBBBXY"));
  CHECK(ReadEquals(base2, "AAAA"));
  CHECK(Missing(base3));
  const auto recovered = rotation.RotateSync();
  CHECK(recovered.has_value());
  CHECK(!rotation.ReopenPending());
  CHECK(rotation.Fd() != live_fd);
  CHECK(rotation.WrittenBytes() == 0);
  CHECK(ReadEquals(base, ""));
  CHECK(ReadEquals(base1, "BBBBXY"));
  CHECK(ReadEquals(base2, "AAAA"));
  CHECK(Missing(base3));
}

void MissingBaseOpensCleanTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  char base2[128]{};
  char base3[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  CHECK(dir.Join(base2, sizeof(base2), "base.2"));
  CHECK(dir.Join(base3, sizeof(base3), "base.3"));
  CHECK(WriteFile(base1, "CCCC"));
  CHECK(WriteFile(base2, "BBBB"));
  CHECK(WriteFile(base3, "AAAA"));
  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 4, .max_files = 3}).has_value());
  CHECK(::unlink(base) == 0);
  NoFileCeiling ceiling;
  CHECK(ceiling.Engage());
  const auto blocked = rotation.RotateSync();
  CHECK(!blocked.has_value());
  CHECK(blocked.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(rotation.ReopenPending());
  ceiling.Release();
  // The retention unlink drops AAAA once. CCCC and BBBB stay put while openat is retried.
  CHECK(Missing(base));
  CHECK(Missing(base1));
  CHECK(ReadEquals(base2, "CCCC"));
  CHECK(ReadEquals(base3, "BBBB"));
  CHECK(ceiling.Engage());
  const auto again = rotation.RotateSync();
  CHECK(!again.has_value());
  ceiling.Release();
  CHECK(Missing(base1));
  CHECK(ReadEquals(base2, "CCCC"));
  CHECK(ReadEquals(base3, "BBBB"));
  const auto recovered = rotation.RotateSync();
  CHECK(recovered.has_value());
  CHECK(!rotation.ReopenPending());
  CHECK(rotation.WrittenBytes() == 0);
  CHECK(ReadEquals(base, ""));
  CHECK(Missing(base1));
  CHECK(ReadEquals(base2, "CCCC"));
  CHECK(ReadEquals(base3, "BBBB"));
}

void RenameResumeKeepsOldestTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  char base2[128]{};
  char base3[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  CHECK(dir.Join(base2, sizeof(base2), "base.2"));
  CHECK(dir.Join(base3, sizeof(base3), "base.3"));
  FaultRotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 8, .max_files = 3}).has_value());
  CHECK(WriteFile(base, "LIVE"));
  CHECK(WriteFile(base1, "ONE"));
  CHECK(WriteFile(base2, "TWO"));
  CHECK(WriteFile(base3, "OLD"));
  g_rename_eperm_slot = 1;
  const auto failed = rotation.RotateSync();
  g_rename_eperm_slot = -1;
  CHECK(!failed.has_value());
  CHECK(failed.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(!rotation.ReopenPending());
  CHECK(rotation.OldestUnlinked());
  CHECK(rotation.NextRenameSlot() == 1);
  CHECK(ReadEquals(base, "LIVE"));
  CHECK(ReadEquals(base1, "ONE"));
  CHECK(Missing(base2));
  CHECK(ReadEquals(base3, "TWO"));
  const auto recovered = rotation.RotateSync();
  CHECK(recovered.has_value());
  CHECK(ReadEquals(base, ""));
  CHECK(ReadEquals(base1, "LIVE"));
  CHECK(ReadEquals(base2, "ONE"));
  CHECK(ReadEquals(base3, "TWO"));
}

void ImmutableRenameOptionalTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  char base2[128]{};
  char base3[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  CHECK(dir.Join(base2, sizeof(base2), "base.2"));
  CHECK(dir.Join(base3, sizeof(base3), "base.3"));
  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 8, .max_files = 3}).has_value());
  CHECK(WriteFile(base, "LIVE"));
  CHECK(WriteFile(base1, "ONE"));
  CHECK(WriteFile(base2, "TWO"));
  CHECK(WriteFile(base3, "OLD"));
  ImmutableFile frozen(base1);
  if (!frozen.Active()) {
    const int err = frozen.ArmErrno();
    if (err == EPERM || err == EACCES || err == ENOTTY || err == EOPNOTSUPP) return;
    CHECK(frozen.Active());
    return;
  }
  const auto failed = rotation.RotateSync();
  CHECK(!failed.has_value());
  CHECK(failed.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(rotation.NextRenameSlot() == 1);
  CHECK(ReadEquals(base3, "TWO"));
  frozen.Clear();
  const auto recovered = rotation.RotateSync();
  CHECK(recovered.has_value());
  CHECK(ReadEquals(base3, "TWO"));
  CHECK(ReadEquals(base2, "ONE"));
}

void AsyncCursorResumeKeepsBaseTest() noexcept {
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  char base2[128]{};
  char base3[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));
  CHECK(dir.Join(base2, sizeof(base2), "base.2"));
  CHECK(dir.Join(base3, sizeof(base3), "base.3"));
  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 8, .max_files = 3}).has_value());
  CHECK(WriteFile(base, "LIVE"));
  CHECK(WriteFile(base1, "ONE"));
  CHECK(WriteFile(base2, "TWO"));
  CHECK(WriteFile(base3, "OLD"));
  CHECK(rotation.NextRenameSlot() == 2);
  CHECK(!rotation.OldestUnlinked());
  const auto early = rotation.OnRenameStepDone(2, 0);
  CHECK(!early.has_value());
  CHECK(early.error() == zerolog::ErrorCode::kInvalidArgument);
  const auto rejected = rotation.OnUnlinkOldestDone(-EPERM);
  CHECK(!rejected.has_value());
  CHECK(rejected.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(!rotation.OldestUnlinked());
  CHECK(rotation.OnUnlinkOldestDone(0).has_value());
  const auto wide = rotation.OnRenameStepDone(15, 0);
  CHECK(!wide.has_value());
  CHECK(wide.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(rotation.NextRenameSlot() == 2);
  CHECK(rotation.OnRenameStepDone(2, 0).has_value());
  CHECK(rotation.NextRenameSlot() == 1);
  const auto blocked = rotation.OnRenameStepDone(1, -EPERM);
  CHECK(!blocked.has_value());
  CHECK(blocked.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(rotation.NextRenameSlot() == 1);
  CHECK(!rotation.ReopenPending());
  const auto recovered = rotation.RotateSync();
  CHECK(recovered.has_value());
  CHECK(ReadEquals(base, ""));
  CHECK(ReadEquals(base1, "LIVE"));
  CHECK(ReadEquals(base2, "ONE"));
  CHECK(ReadEquals(base3, "OLD"));
}

void BadPolicyOpenRejectedTest() noexcept {
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  zerolog::Rotation naked;
  CHECK(!naked.RotateSync().has_value());
  CHECK(naked.RotateSync().error() == zerolog::ErrorCode::kInvalidArgument);
  zerolog::Rotation rotation;
  const auto zero_files = rotation.Open(base, zerolog::RotationPolicy{.max_files = 0});
  CHECK(!zero_files.has_value());
  CHECK(zero_files.error() == zerolog::ErrorCode::kInvalidArgument);
  const auto wide = rotation.Open(base, zerolog::RotationPolicy{.max_files = 16});
  CHECK(!wide.has_value());
  CHECK(wide.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(Missing(base));
  const auto opened = rotation.Open(base, zerolog::RotationPolicy{.max_files = 15});
  CHECK(opened.has_value());
  const auto again = rotation.Open(base, zerolog::RotationPolicy{.max_files = 3});
  CHECK(!again.has_value());
  CHECK(again.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(rotation.Fd() >= 0);
}

void SqeAppendOffsetPathsTest() noexcept {
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 32, .max_files = 3}).has_value());
  const std::byte payload[]{std::byte{'Q'}, std::byte{'R'}};
  uring::abi::FlatSqe image{};
  rotation.PrepareWrite(payload, 2).Encode(image);
  CHECK(image.opcode == uring::abi::kOpWrite);
  CHECK(image.fd == rotation.Fd());
  CHECK(image.offset == ~std::uint64_t{0});
  CHECK(image.len == 2);
  CHECK(image.address == reinterpret_cast<std::uint64_t>(payload));

  rotation.PrepareRenameStep(0).Encode(image);
  CHECK(image.opcode == uring::abi::kOpRenameat);
  CHECK(image.fd == AT_FDCWD);
  CHECK(image.len == static_cast<std::uint32_t>(AT_FDCWD));
  CHECK(image.address == reinterpret_cast<std::uint64_t>(rotation.PathAt(0)));
  CHECK(image.offset == reinterpret_cast<std::uint64_t>(rotation.PathAt(1)));
  char archive_one[160]{};
  char archive_three[160]{};
  std::snprintf(archive_one, sizeof(archive_one), "%s.1", base);
  std::snprintf(archive_three, sizeof(archive_three), "%s.3", base);
  CHECK(std::strcmp(rotation.PathAt(0), base) == 0);
  CHECK(std::strcmp(rotation.PathAt(1), archive_one) == 0);

  rotation.PrepareRenameStep(2).Encode(image);
  CHECK(image.address == reinterpret_cast<std::uint64_t>(rotation.PathAt(2)));
  CHECK(image.offset == reinterpret_cast<std::uint64_t>(rotation.PathAt(3)));

  rotation.PrepareOpenNew().Encode(image);
  CHECK(image.opcode == uring::abi::kOpOpenat);
  CHECK(image.fd == AT_FDCWD);
  CHECK(image.address == reinterpret_cast<std::uint64_t>(rotation.PathAt(0)));
  CHECK(image.len == 0644);
  const auto flags = static_cast<std::uint32_t>(O_WRONLY) | static_cast<std::uint32_t>(O_CREAT) |
                     static_cast<std::uint32_t>(O_TRUNC) | static_cast<std::uint32_t>(O_APPEND) |
                     static_cast<std::uint32_t>(O_CLOEXEC);
  CHECK(image.operation_flags == flags);

  rotation.PrepareUnlinkOldest().Encode(image);
  CHECK(image.opcode == uring::abi::kOpUnlinkat);
  CHECK(image.fd == AT_FDCWD);
  CHECK(image.address == reinterpret_cast<std::uint64_t>(rotation.PathAt(3)));
  CHECK(std::strcmp(rotation.PathAt(3), archive_three) == 0);
}

}  // namespace

int main() {
  static_assert(!std::is_copy_constructible_v<zerolog::Rotation>);
  static_assert(!std::is_move_constructible_v<zerolog::Rotation>);
  static_assert(sizeof(zerolog::RotationPolicy) == 16);
  static_assert(sizeof(zerolog::WriteProgress) == 8);
  GenerationsKeepExactBytesTest();
  EintrPartialKeepsPrefixTest();
  EintrThenFullCommitTest();
  ZeroReturnReportsShortTest();
  BackoffWriteStaysSuccessTest();
  MissingBaseOpensCleanTest();
  RenameResumeKeepsOldestTest();
  ImmutableRenameOptionalTest();
  AsyncCursorResumeKeepsBaseTest();
  BadPolicyOpenRejectedTest();
  SqeAppendOffsetPathsTest();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d rotation checks failed\n", g_failures);
    return 1;
  }
  std::printf("rotation_test ok\n");
  return 0;
}
