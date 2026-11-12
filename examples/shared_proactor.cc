#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <uring/core/dispatcher.hpp>
#include <uring/core/proactor.hpp>
#include <zerolog/zerolog.hpp>

namespace {

[[nodiscard]] int Code(zerolog::ErrorCode code) noexcept { return static_cast<int>(code); }

[[nodiscard]] bool WindowContains(const char* data, std::size_t size, const char* needle, std::size_t needle_len) noexcept {
  if (needle_len == 0 || size < needle_len) return false;
  for (std::size_t i = 0; i + needle_len <= size; ++i) {
    if (std::memcmp(data + i, needle, needle_len) == 0) return true;
  }
  return false;
}

[[nodiscard]] int FileHasRecord(const char* path, const char* needle) noexcept {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::fprintf(stderr, "open %s errno=%d\n", path, errno);
    return Code(zerolog::ErrorCode::kWriteFailed);
  }
  const std::size_t needle_len = std::strlen(needle);
  char window[4096];
  std::size_t filled = 0;
  bool any = false;
  for (;;) {
    const ssize_t n = ::read(fd, window + filled, sizeof(window) - filled);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "read %s errno=%d\n", path, errno);
      ::close(fd);
      return Code(zerolog::ErrorCode::kWriteFailed);
    }
    if (n == 0) break;
    any = true;
    filled += static_cast<std::size_t>(n);
    if (WindowContains(window, filled, needle, needle_len)) {
      ::close(fd);
      return 0;
    }
    if (filled >= needle_len && needle_len > 0) {
      const std::size_t keep = needle_len - 1U;
      std::memmove(window, window + filled - keep, keep);
      filled = keep;
    }
  }
  ::close(fd);
  std::fprintf(stderr, any ? "log file missing expected record\n" : "log file is empty\n");
  return Code(zerolog::ErrorCode::kWriteFailed);
}

[[nodiscard]] int RemoveFile(const char* path) noexcept {
  if (::unlink(path) == 0 || errno == ENOENT) return 0;
  std::fprintf(stderr, "unlink %s errno=%d\n", path, errno);
  return Code(zerolog::ErrorCode::kWriteFailed);
}

[[nodiscard]] int RemoveExampleDir(const char* directory, const char* path) noexcept {
  char archived[160];
  int status = RemoveFile(path);
  for (int index = 1; index <= 2; ++index) {
    const int wrote = std::snprintf(archived, sizeof(archived), "%s.%d", path, index);
    if (wrote < 0 || static_cast<std::size_t>(wrote) >= sizeof(archived)) return Code(zerolog::ErrorCode::kInvalidArgument);
    if (status == 0) status = RemoveFile(archived);
  }
  if (::rmdir(directory) != 0 && errno != ENOENT && status == 0) {
    std::fprintf(stderr, "rmdir %s errno=%d\n", directory, errno);
    status = Code(zerolog::ErrorCode::kWriteFailed);
  }
  return status;
}

void LogReady() noexcept {
  zerolog::PrewarmThread();
  zerolog::Info("shared proactor ready value={}", 7U);
}

template <class Dispatcher>
[[nodiscard]] int CloseEngine(uring::Proactor& engine, Dispatcher& dispatcher) noexcept {
  while (engine.Stage() != uring::DrainStage::kClosed) {
    if (engine.ShutdownWith(dispatcher, std::chrono::milliseconds(100))) break;
  }
  if (engine.Stage() != uring::DrainStage::kClosed) return Code(zerolog::ErrorCode::kConsumerBusy);
  return 0;
}

[[nodiscard]] int RunShared(const char* path) noexcept {
  zerolog::Rotation rotation{};
  auto opened = rotation.Open(path, zerolog::RotationPolicy{.max_bytes = 64U * 1024U, .max_files = 2U});
  if (!opened) return Code(opened.error());
  auto consumer = zerolog::AcquireConsumer();
  if (!consumer) return Code(consumer.error());

  auto engine = uring::Proactor::Create(64U, uring::RingMode::kStandard64_16, zerolog::ProactorLogSink<>::RequiredCaps());
  if (!engine) {
    const auto code = zerolog::FromUring(engine.error());
    if (code != zerolog::ErrorCode::kProactorUnavailable) return Code(code);
    zerolog::FutexFallbackSink<> fallback{*consumer, rotation};
    LogReady();
    auto flushed = fallback.ShutdownFlushSync();
    if (!flushed) return Code(flushed.error());
    return FileHasRecord(path, "shared proactor ready value=7");
  }

  zerolog::ProactorLogSink<> sink{*engine, *consumer, rotation};
  uring::SingleDispatcher dispatcher{sink};
  engine->BindDispatcher(dispatcher);

  int status = 0;
  auto started = sink.Start();
  if (!started) {
    status = Code(started.error());
  } else {
    LogReady();
  }
  auto stopped = sink.RequestStop();
  if (!stopped && status == 0) status = Code(stopped.error());
  if (const int closed = CloseEngine(*engine, dispatcher); closed != 0 && status == 0) status = closed;
  if (engine->Stage() == uring::DrainStage::kClosed && sink.Quiescent()) {
    auto flushed = sink.ShutdownFlushSync();
    if (!flushed && status == 0) status = Code(flushed.error());
  } else if (status == 0) {
    status = Code(zerolog::ErrorCode::kConsumerBusy);
  }
  if (status != 0) return status;
  return FileHasRecord(path, "shared proactor ready value=7");
}

}  // namespace

int main() {
  char directory[] = "/tmp/zerolog-example-mode-a-XXXXXX";
  if (::mkdtemp(directory) == nullptr) {
    std::fprintf(stderr, "mkdtemp errno=%d\n", errno);
    return Code(zerolog::ErrorCode::kWriteFailed);
  }
  char path[128];
  const int wrote = std::snprintf(path, sizeof(path), "%s/app.log", directory);
  if (wrote < 0 || static_cast<std::size_t>(wrote) >= sizeof(path)) {
    ::rmdir(directory);
    return Code(zerolog::ErrorCode::kInvalidArgument);
  }
  const int status = RunShared(path);
  const int cleanup = RemoveExampleDir(directory, path);
  return status != 0 ? status : cleanup;
}
