#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <zerolog/zerolog.hpp>

namespace {
enum class Lane : std::uint8_t { kAlpha = 1, kBeta = 2 };
struct Sample {
  std::uint32_t timestamp;
  std::uint32_t count;
};

struct Worker {
  std::uint32_t index = 0;
  std::uint32_t code = static_cast<std::uint32_t>(zerolog::ErrorCode::kSuccess);
};

void* Produce(void* raw) noexcept {
  auto* worker = static_cast<Worker*>(raw);
  auto warmed = zerolog::TryPrewarmThread();
  if (!warmed) {
    worker->code = static_cast<std::uint32_t>(warmed.error());
    return nullptr;
  }
  const zerolog::SpanGuard span{static_cast<std::uint64_t>(worker->index) + 1U};
  const Lane lane = (worker->index & 1U) == 0U ? Lane::kAlpha : Lane::kBeta;
  const Sample sample{100U + worker->index, 10U + worker->index};
  zerolog::Info("lane={} sample={}", lane, sample);
  if (worker->index == 0U) {
    const auto gated = zerolog::IfEnabled(zerolog::Level::kInfo, []() noexcept { zerolog::Info("enabled={}", 1U); });
    worker->code = static_cast<std::uint32_t>(gated);
  }
  return nullptr;
}

void* NarrowStack(void* raw) noexcept {
  auto* code = static_cast<std::uint32_t*>(raw);
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) != 0) {
    *code = static_cast<std::uint32_t>(zerolog::ErrorCode::kInvalidArgument);
    return nullptr;
  }
  std::size_t stack = 0;
  const int size_status = pthread_attr_getstacksize(&attr, &stack);
  pthread_attr_destroy(&attr);
  if (size_status != 0 || stack < 128U * 1024U || stack >= 256U * 1024U) {
    std::fprintf(stderr, "narrow stack size=%zu\n", stack);
    *code = static_cast<std::uint32_t>(zerolog::ErrorCode::kInvalidArgument);
    return nullptr;
  }
  auto warmed = zerolog::TryPrewarmThread();
  if (!warmed) {
    *code = static_cast<std::uint32_t>(warmed.error());
    return nullptr;
  }
  *code = static_cast<std::uint32_t>(zerolog::TryInfo("narrow stack"));
  return nullptr;
}

[[nodiscard]] int JoinAll(pthread_t* threads, int started) noexcept {
  int first = 0;
  for (int i = 0; i < started; ++i) {
    const int joined = pthread_join(threads[i], nullptr);
    if (joined != 0 && first == 0) first = joined;
  }
  return first;
}

[[nodiscard]] int RemovePath(const char* path) noexcept {
  if (unlink(path) == 0 || errno == ENOENT) return 0;
  std::fprintf(stderr, "unlink %s errno=%d\n", path, errno);
  return static_cast<int>(zerolog::ErrorCode::kWriteFailed);
}
}  // namespace

int main() {
  char directory[] = "/tmp/zerolog-pipeline-e2e-XXXXXX";
  if (mkdtemp(directory) == nullptr) {
    std::fprintf(stderr, "mkdtemp errno=%d\n", errno);
    return static_cast<int>(zerolog::ErrorCode::kWriteFailed);
  }
  char path[96];
  std::snprintf(path, sizeof(path), "%s/log", directory);

  zerolog::SetLevel(zerolog::Level::kInfo);
  constexpr int producers = 16;
  Worker workers[producers]{};
  pthread_t threads[producers + 1]{};
  int started = 0;
  for (int i = 0; i < producers; ++i) {
    workers[i].index = static_cast<std::uint32_t>(i);
    const int created = pthread_create(&threads[started], nullptr, Produce, &workers[i]);
    if (created != 0) {
      std::fprintf(stderr, "pthread_create errno=%d\n", created);
      if (const int joined = JoinAll(threads, started); joined != 0) {
        std::fprintf(stderr, "pthread_join errno=%d\n", joined);
        return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
      }
      return static_cast<int>(created == EAGAIN ? zerolog::ErrorCode::kTooManyThreads : zerolog::ErrorCode::kInvalidArgument);
    }
    ++started;
  }

  pthread_attr_t narrow_attr;
  auto narrow_code = static_cast<std::uint32_t>(zerolog::ErrorCode::kSuccess);
  if (pthread_attr_init(&narrow_attr) != 0 || pthread_attr_setstacksize(&narrow_attr, 128U * 1024U) != 0) {
    if (const int joined = JoinAll(threads, started); joined != 0) {
      std::fprintf(stderr, "pthread_join errno=%d\n", joined);
      return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
    }
    std::fputs("pthread stack attribute failed\n", stderr);
    return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
  }
  const int narrow_created = pthread_create(&threads[started], &narrow_attr, NarrowStack, &narrow_code);
  pthread_attr_destroy(&narrow_attr);
  if (narrow_created != 0) {
    if (const int joined = JoinAll(threads, started); joined != 0) {
      std::fprintf(stderr, "pthread_join errno=%d\n", joined);
      return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
    }
    std::fprintf(stderr, "narrow pthread_create errno=%d\n", narrow_created);
    return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
  }
  ++started;
  if (const int joined = JoinAll(threads, started); joined != 0) {
    std::fprintf(stderr, "pthread_join errno=%d\n", joined);
    return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
  }
  for (const Worker& worker : workers) {
    if (worker.code != static_cast<std::uint32_t>(zerolog::ErrorCode::kSuccess)) return static_cast<int>(worker.code);
  }
  if (narrow_code != static_cast<std::uint32_t>(zerolog::ErrorCode::kSuccess)) return static_cast<int>(narrow_code);

  zerolog::Rotation rotation;
  auto opened = rotation.Open(path, zerolog::RotationPolicy{4096, 2});
  if (!opened) return static_cast<int>(opened.error());
  auto consumer = zerolog::AcquireConsumer();
  if (!consumer) return static_cast<int>(consumer.error());
  if (const auto set = consumer->SetOutputFd(rotation.Fd()); set != zerolog::ErrorCode::kSuccess) return static_cast<int>(set);
  auto drained = consumer->Poll(true);
  if (!drained) return static_cast<int>(drained.error());
  if (drained->records == 0) {
    std::fputs("pipeline drain produced no records\n", stderr);
    return static_cast<int>(zerolog::ErrorCode::kWriteFailed);
  }
  auto rotated = rotation.RotateSync();
  if (!rotated) return static_cast<int>(rotated.error());
  const char marker[] = "rotated\n";
  const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(marker), sizeof(marker) - 1);
  const auto progress = rotation.WriteSync(bytes);
  if (progress.error != zerolog::ErrorCode::kSuccess) return static_cast<int>(progress.error);

  char archived[112];
  std::snprintf(archived, sizeof(archived), "%s.1", path);
  int cleanup = RemovePath(path);
  if (cleanup == 0) cleanup = RemovePath(archived);
  char oldest[112];
  std::snprintf(oldest, sizeof(oldest), "%s.2", path);
  if (cleanup == 0) cleanup = RemovePath(oldest);
  if (rmdir(directory) != 0 && errno != ENOENT && cleanup == 0) {
    std::fprintf(stderr, "rmdir %s errno=%d\n", directory, errno);
    cleanup = static_cast<int>(zerolog::ErrorCode::kWriteFailed);
  }
  return cleanup;
}
