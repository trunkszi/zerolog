/** @file @brief Unit tests for `FutexFallbackSink` and `RunFutexFallback`. */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <zerolog/abi/contract.hpp>
#include <zerolog/core/consumer_core.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/lease.hpp>
#include <zerolog/core/producer.hpp>
#include <zerolog/core/registry.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/sink/futex_fallback.hpp>
#include <zerolog/sink/rotation.hpp>

namespace {

using namespace zerolog;
using namespace zerolog::sink;

bool Check(bool cond, const char* expr, int line) {
  if (!cond) {
    (void)::write(STDERR_FILENO, "FAIL line ", 10);
    char buf[16]{};
    int pos = 15;
    int v = line;
    buf[pos] = '\n';
    while (v > 0 && pos > 0) {
      buf[--pos] = static_cast<char>('0' + (v % 10));
      v /= 10;
    }
    (void)::write(STDERR_FILENO, buf + pos, static_cast<std::size_t>(16 - pos));
    (void)::write(STDERR_FILENO, expr, std::strlen(expr));
    (void)::write(STDERR_FILENO, "\n", 1);
  }
  return cond;
}

#define CHECK(expr)                        \
  do {                                     \
    if (!Check((expr), #expr, __LINE__)) { \
      return false;                        \
    }                                      \
  } while (false)

struct TempDir {
  std::filesystem::path dir{};

  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "zerolog_futex_XXXXXX").string();
    if (::mkdtemp(tmpl.data()) != nullptr) {
      dir = tmpl;
    }
  }

  ~TempDir() {
    if (!dir.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
  }
};

std::string ReadWholeFile(const std::filesystem::path& p) {
  const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  std::string out;
  char buf[512];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  (void)::close(fd);
  return out;
}

struct ResetGuard {
  ResetGuard() noexcept { Reset(); }
  ~ResetGuard() noexcept { Reset(); }

  static void Reset() noexcept {
    core::ResetLeaseStateForTesting();
    core::ResetDoorbellForTesting();
    core::SetQueueFullPolicy(abi::QueueFullPolicy::kDrop);
    core::SetLevel(abi::Level::kTrace);
    auto& reg = core::GlobalRegistry();
    for (std::uint32_t slot = 0; slot < core::ThreadRegistry::kMaxSlots; ++slot) {
      if (auto* buf = reg.GetSlotBuffer(slot); buf != nullptr) {
        for (;;) {
          const auto view = buf->queue.Front();
          if (!view.Valid()) break;
          buf->queue.Pop(view.header.get<^^abi::RecordHeaderSpec::blocks>());
        }
      }
    }
  }
};

int g_fail_write_errno = 0;
std::uint32_t g_partial_write_bytes = 0;
int g_fail_rename_errno = 0;
int g_fail_open_on_call = -1;
int g_open_calls = 0;

void ResetFaults() noexcept {
  g_fail_write_errno = 0;
  g_partial_write_bytes = 0;
  g_fail_rename_errno = 0;
  g_fail_open_on_call = -1;
  g_open_calls = 0;
}

struct FaultIo {
  [[nodiscard]] static std::int64_t Write(std::int32_t fd, const std::byte* data, std::uint32_t length) noexcept {
    if (g_partial_write_bytes > 0U && length > g_partial_write_bytes) {
      const std::uint32_t n = g_partial_write_bytes;
      g_partial_write_bytes = 0U;
      return PosixIo::Write(fd, data, n);
    }
    if (g_fail_write_errno != 0) {
      return -static_cast<std::int64_t>(g_fail_write_errno);
    }
    return PosixIo::Write(fd, data, length);
  }

  [[nodiscard]] static std::int64_t Open(const char* path, std::uint32_t flags, std::uint32_t mode) noexcept {
    ++g_open_calls;
    if (g_fail_open_on_call > 0 && g_open_calls == g_fail_open_on_call) {
      return -static_cast<std::int64_t>(EACCES);
    }
    return PosixIo::Open(path, flags, mode);
  }

  [[nodiscard]] static std::int64_t UnlinkAt(int dirfd, const char* pathname, int flags) noexcept {
    return PosixIo::UnlinkAt(dirfd, pathname, flags);
  }

  [[nodiscard]] static std::int64_t RenameAt(int olddirfd, const char* oldpath, int newdirfd, const char* newpath,
                                             unsigned int flags) noexcept {
    if (g_fail_rename_errno != 0) {
      return -static_cast<std::int64_t>(g_fail_rename_errno);
    }
    return PosixIo::RenameAt(olddirfd, oldpath, newdirfd, newpath, flags);
  }
};

using FaultRotation = BasicRotation<FaultIo>;

bool TestPartialWriteBackoffAndIndependentRotationBackoff() {
  ResetGuard guard;
  ResetFaults();
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  const auto base = tmp.dir / "fallback.log";
  FaultRotation rotation;
  CHECK(rotation.Open(base.string(), RotationPolicy{.max_bytes = 96U, .max_files = 2U}).has_value());

  auto core = core::ConsumerCore::Create();
  CHECK(core.has_value());

  FutexFallbackSink<FaultRotation> sink{
      *core,
      rotation,
      SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 5'000'000ULL, .manage_deep_park = 0U},
  };

  zerolog::Info("fallback partial write {}", 101U);

  // Inject a partial write (16 bytes written) followed by ENOSPC on the next write syscall in the same WriteSync loop.
  g_partial_write_bytes = 16U;
  g_fail_write_errno = ENOSPC;

  auto step1 = sink.Step(1'000'000ULL);
  CHECK(step1.has_value());
  CHECK(sink.WriteRetryActive());
  CHECK(sink.WriteBackoffMs() == 1U);
  CHECK(!sink.RotationRetryActive());
  CHECK(sink.LastError() == ErrorCode::kWriteFailed);
  CHECK(sink.TotalUnflushedBytes() > 0U);
  const std::string prefix = ReadWholeFile(base);
  CHECK(prefix.size() == 16U);

  // Before the 1 ms backoff deadline expires, Step does not retry the write.
  auto step_early = sink.Step(1'500'000ULL);
  CHECK(step_early.has_value());
  CHECK(sink.WriteRetryActive());

  // At 2 ms (deadline expired), clear write fault, inject rotation rename failure, and emit enough records to cross
  // 96B.
  g_fail_write_errno = 0;
  g_fail_rename_errno = EACCES;
  zerolog::Info("fallback second record {}", 202U);

  auto step2 = sink.Step(2'000'000ULL);
  CHECK(step2.has_value());
  CHECK(!sink.WriteRetryActive());
  CHECK(sink.WriteBackoffMs() == 0U);
  CHECK(sink.RotationRetryActive());
  CHECK(sink.RotationBackoffMs() == 1U);
  CHECK(sink.TotalUnflushedBytes() == 0U);

  // Clear rotation fault and step past rotation deadline (3 ms).
  g_fail_rename_errno = 0;
  zerolog::Info("fallback third record {}", 303U);
  auto step3 = sink.Step(3'500'000ULL);
  CHECK(step3.has_value());
  CHECK(!sink.RotationRetryActive());
  CHECK(sink.RotationBackoffMs() == 0U);

  const std::string archive2 = ReadWholeFile(std::filesystem::path(base.string() + ".2"));
  const std::string archive1 = ReadWholeFile(std::filesystem::path(base.string() + ".1"));
  const std::string active = ReadWholeFile(base);
  const std::string combined = archive2 + archive1 + active;
  CHECK(combined.find("fallback partial write 101") != std::string::npos);
  CHECK(combined.find("fallback second record 202") != std::string::npos);
  CHECK(combined.find("fallback third record 303") != std::string::npos);
  return true;
}

bool TestOpenOnlyRecoveryWritesToRetainedBase1() {
  ResetGuard guard;
  ResetFaults();
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  const auto base = tmp.dir / "reopen.log";
  FaultRotation rotation;
  // Open call #1 is initial Open; Open call #2 (reopen after rename base -> base.1) will fail with EACCES.
  g_fail_open_on_call = 2;
  CHECK(rotation.Open(base.string(), RotationPolicy{.max_bytes = 40U, .max_files = 2U}).has_value());

  auto core = core::ConsumerCore::Create();
  CHECK(core.has_value());

  FutexFallbackSink<FaultRotation> sink{
      *core,
      rotation,
      SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 5'000'000ULL, .manage_deep_park = 0U},
  };

  zerolog::Info("trigger reopen failure {}", 1U);
  CHECK(sink.Step(1'000'000ULL).has_value());
  CHECK(rotation.ReopenPending());
  CHECK(rotation.IsOpen());
  CHECK(sink.RotationRetryActive());

  // While rotation_retry_ is still active (before 2 ms), new writes continue appending to the retained base.1 fd.
  zerolog::Info("written while reopen pending {}", 2U);
  CHECK(sink.Step(1'200'000ULL).has_value());
  CHECK(sink.TotalUnflushedBytes() == 0U);

  const std::string archived = ReadWholeFile(std::filesystem::path(base.string() + ".1"));
  CHECK(archived.find("trigger reopen failure 1") != std::string::npos);
  CHECK(archived.find("written while reopen pending 2") != std::string::npos);

  // Advance past the 1 ms rotation backoff deadline: Open-only recovery opens the fresh `base` without re-renaming.
  zerolog::Info("fresh file after reopen {}", 3U);
  CHECK(sink.Step(2'500'000ULL).has_value());
  CHECK(!rotation.ReopenPending());
  CHECK(!sink.RotationRetryActive());

  const std::string fresh = ReadWholeFile(std::filesystem::path(base.string() + ".1")) + ReadWholeFile(base);
  CHECK(fresh.find("fresh file after reopen 3") != std::string::npos);
  return true;
}

bool TestConcurrentFutexWaitAndDoorbellWake() {
  ResetGuard guard;
  ResetFaults();
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  const auto base = tmp.dir / "concurrent.log";
  Rotation rotation;
  CHECK(rotation.Open(base.string(), RotationPolicy{.max_bytes = 1024U, .max_files = 3U}).has_value());

  auto core = core::ConsumerCore::Create();
  CHECK(core.has_value());

  std::atomic<bool> stop_requested{false};
  Result<void> run_result{};

  std::thread consumer_thread([&] {
    run_result = RunFutexFallback(
        *core, rotation, stop_requested,
        SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 50'000'000ULL, .manage_deep_park = 1U});
  });

  for (std::uint32_t i = 0; i < 24U; ++i) {
    zerolog::Info("futex loop message {}", i);
    if ((i & 3U) == 0U) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  stop_requested.store(true, std::memory_order_release);
  core::RingDoorbellUrgentCold();
  consumer_thread.join();

  CHECK(run_result.has_value());

  std::string all = ReadWholeFile(std::filesystem::path(base.string() + ".3")) +
                    ReadWholeFile(std::filesystem::path(base.string() + ".2")) +
                    ReadWholeFile(std::filesystem::path(base.string() + ".1")) + ReadWholeFile(base);
  CHECK(all.find("futex loop message 0") != std::string::npos);
  CHECK(all.find("futex loop message 23") != std::string::npos);
  return true;
}

bool TestShutdownFlushWritesBufferedLogsOnRotationFailure() {
  ResetGuard guard;
  ResetFaults();
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  const auto base = tmp.dir / "shutdown_rot_fail.log";
  FaultRotation rotation;
  CHECK(rotation.Open(base.string(), RotationPolicy{.max_bytes = 16U, .max_files = 2U}).has_value());

  auto core = core::ConsumerCore::Create();
  CHECK(core.has_value());

  FutexFallbackSink<FaultRotation> sink{
      *core,
      rotation,
      SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 5'000'000ULL, .manage_deep_park = 1U},
  };

  g_fail_rename_errno = EACCES;
  zerolog::Info("futex_shutdown_rot_fail_1={}", 101U);
  zerolog::Info("futex_shutdown_rot_fail_2={}", 202U);

  const auto flushed = sink.ShutdownFlushSync();
  g_fail_rename_errno = 0;
  CHECK(!flushed.has_value());
  CHECK(flushed.error() == ErrorCode::kRotateFailed);
  CHECK(sink.TotalUnflushedBytes() == 0U);

  const std::string content = ReadWholeFile(base);
  CHECK(content.find("futex_shutdown_rot_fail_1=101") != std::string::npos);
  CHECK(content.find("futex_shutdown_rot_fail_2=202") != std::string::npos);
  return true;
}

}  // namespace

int main() {
  if (!TestPartialWriteBackoffAndIndependentRotationBackoff()) return EXIT_FAILURE;
  if (!TestOpenOnlyRecoveryWritesToRetainedBase1()) return EXIT_FAILURE;
  if (!TestConcurrentFutexWaitAndDoorbellWake()) return EXIT_FAILURE;
  if (!TestShutdownFlushWritesBufferedLogsOnRotationFailure()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
