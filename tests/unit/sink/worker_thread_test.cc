/** @file @brief Unit tests for Mode B `UringConsumerThread` (512 KiB stack, io_uring loop, and futex2 fallback). */

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <uring/error/uring_error.hpp>
#include <zerolog/abi/contract.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/lease.hpp>
#include <zerolog/core/producer.hpp>
#include <zerolog/core/registry.hpp>
#include <zerolog/error/zerolog_error.hpp>
#include <zerolog/sink/rotation.hpp>
#include <zerolog/sink/worker_thread.hpp>

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
    std::string tmpl = (std::filesystem::temp_directory_path() / "zerolog_worker_XXXXXX").string();
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

struct SmallCallerStackCtx {
  std::filesystem::path log_path{};
  bool ok{false};
};

void* RunFromSmallCallerStack(void* arg) noexcept {
  auto* ctx = static_cast<SmallCallerStackCtx*>(arg);
  UringConsumerThread<> worker;
  auto started = worker.Start(ctx->log_path.string(), RotationPolicy{.max_bytes = 256U, .max_files = 2U},
                              SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 2'000'000ULL});
  if (!started) {
    return nullptr;
  }
  if (!worker.Running() || worker.WorkerStackSize() < UringConsumerThread<>::kWorkerStackBytes) {
    [[maybe_unused]] const auto stopped = worker.Stop();
    return nullptr;
  }
  for (std::uint32_t i = 0; i < 8U; ++i) {
    if (zerolog::TryInfo("small stack caller msg {}", i) != ErrorCode::kSuccess) {
      [[maybe_unused]] const auto stopped = worker.Stop();
      return nullptr;
    }
  }
  auto stopped = worker.Stop();
  ctx->ok = stopped.has_value() && !worker.Running();
  return nullptr;
}

bool TestWorker512KiBStackAndSmallCallerStack() {
  ResetGuard guard;
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  SmallCallerStackCtx ctx{.log_path = tmp.dir / "small_caller.log", .ok = false};
  pthread_attr_t attr{};
  CHECK(::pthread_attr_init(&attr) == 0);
  // Spawn caller thread with a 128 KiB stack to prove UringConsumerThread allocates its Proactor on the 512 KiB worker
  // stack and never on the caller stack.
  CHECK(::pthread_attr_setstacksize(&attr, 128U * 1024U) == 0);
  pthread_t caller{};
  CHECK(::pthread_create(&caller, &attr, &RunFromSmallCallerStack, &ctx) == 0);
  ::pthread_attr_destroy(&attr);
  CHECK(::pthread_join(caller, nullptr) == 0);
  CHECK(ctx.ok);

  const std::string combined = ReadWholeFile(std::filesystem::path(ctx.log_path.string() + ".1")) + ReadWholeFile(ctx.log_path);
  CHECK(combined.find("small stack caller msg 0") != std::string::npos);
  CHECK(combined.find("small stack caller msg 7") != std::string::npos);
  return true;
}

bool TestFallbackOnAllFiveProactorUnavailableErrors() {
  ResetGuard guard;
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  constexpr std::array<uring::UringErrorCode, 5> unavailable_codes = {
      uring::UringErrorCode::kPermissionDenied,
      uring::UringErrorCode::kNotImplemented,
      uring::UringErrorCode::kNotSupported,
      uring::UringErrorCode::kAccessDenied,
      uring::UringErrorCode::kUnsupportedKernelVersion,
  };

  for (std::size_t idx = 0; idx < unavailable_codes.size(); ++idx) {
    const auto path = tmp.dir / ("fallback_" + std::to_string(idx) + ".log");
    UringConsumerThread<> worker;
    const std::string path_str = path.string();
    auto started = worker.Start(WorkerConfig{
        .base_path = path_str,
        .rotation = RotationPolicy{.max_bytes = 512U, .max_files = 2U},
        .sink = SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 5'000'000ULL},
        .ring_entries = 64U,
        .fault_uring = unavailable_codes[idx],
    });
    CHECK(started.has_value());
    CHECK(worker.Running());
    CHECK(worker.Backend() == WorkerBackend::kFutexFallback);
    CHECK(worker.WorkerStackSize() >= UringConsumerThread<>::kWorkerStackBytes);

    zerolog::Info("fallback record idx={}", static_cast<std::uint32_t>(idx));
    CHECK(worker.Stop().has_value());
    CHECK(!worker.Running());

    const std::string content = ReadWholeFile(path);
    CHECK(content.find("fallback record idx=") != std::string::npos);
  }
  return true;
}

bool TestNonFallbackErrorAndLeaseContention() {
  ResetGuard guard;
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  const auto path = (tmp.dir / "err.log").string();
  UringConsumerThread<> worker;

  // Invalid config arguments return ErrorCode::kInvalidArgument.
  CHECK(worker.Start(WorkerConfig{.base_path = "", .ring_entries = 64U, .fault_uring = std::nullopt}).error() ==
        ErrorCode::kInvalidArgument);
  CHECK(worker.Start(WorkerConfig{.base_path = path, .ring_entries = 33U, .fault_uring = std::nullopt}).error() ==
        ErrorCode::kInvalidArgument);

  // Non-fallback UringErrorCode (e.g. kInvalid -> ErrorCode::kInvalidArgument) fails Start() and releases the lease.
  auto bad = worker.Start(WorkerConfig{
      .base_path = path,
      .rotation = RotationPolicy{.max_bytes = 1024U, .max_files = 2U},
      .sink = SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 5'000'000ULL},
      .ring_entries = 64U,
      .fault_uring = uring::UringErrorCode::kInvalid,
  });
  CHECK(!bad.has_value());
  CHECK(bad.error() == ErrorCode::kInvalidArgument);
  CHECK(!worker.Running());

  // Hold an external consumer lease and verify UringConsumerThread::Start returns ErrorCode::kLeaseHeld.
  {
    auto ext_lease = core::TryAcquireExternalLease();
    CHECK(ext_lease.has_value());
    auto contended = worker.Start(path);
    CHECK(!contended.has_value());
    CHECK(contended.error() == ErrorCode::kLeaseHeld);
  }

  // Start worker normally, verify double-Start returns kConsumerBusy, and external lease acquisition returns
  // kConsumerBusy.
  CHECK(worker.Start(path).has_value());
  CHECK(worker.Start(path).error() == ErrorCode::kConsumerBusy);
  CHECK(core::TryAcquireExternalLease().error() == ErrorCode::kConsumerBusy);
  CHECK(worker.Stop().has_value());
  CHECK(worker.Stop().error() == ErrorCode::kInvalidArgument);
  return true;
}

bool TestIdleWorkerEventDrivenAndImmediateStop() {
  ResetGuard guard;
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  const auto path = (tmp.dir / "idle.log").string();
  UringConsumerThread<> worker;
  CHECK(worker
            .Start(path, RotationPolicy{.max_bytes = 4096U, .max_files = 2U},
                   SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 100'000'000ULL})
            .has_value());

  // Remain completely idle for 150 ms across a 100 ms idle_timeout_ns window, then emit a record and stop immediately.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  zerolog::Info("event_driven_idle_ok={}", 77U);

  const auto stop_start = std::chrono::steady_clock::now();
  CHECK(worker.Stop().has_value());
  const auto stop_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stop_start);
  // Stop() wakes the worker immediately via RingDoorbellUrgentCold(), never waiting for the 100 ms idle timer.
  CHECK(stop_elapsed.count() < 80);

  const std::string content = ReadWholeFile(path);
  CHECK(content.find("event_driven_idle_ok=77") != std::string::npos);
  return true;
}

bool TestBuilderAndLoggerGuardWithFileAndPipeFd() {
  ResetGuard guard;
  TempDir tmp;
  CHECK(!tmp.dir.empty());

  const auto path = (tmp.dir / "builder_file.log").string();
  {
    auto logger_guard = Builder()
                            .Level(abi::Level::kInfo)
                            .LogFile(path)
                            .Rotation(RotationPolicy{.max_bytes = 256U, .max_files = 2U})
                            .FlushOn(abi::Level::kError)
                            .IdleTimeout(std::chrono::milliseconds(5))
                            .Start();
    CHECK(logger_guard.has_value());
    CHECK(logger_guard->Active());
    zerolog::Debug("filtered debug record {}", 1U);
    zerolog::Info("builder file record {}", 42U);
    zerolog::Warn("builder warn record {:x}", 0xBEEFU);
    // LoggerGuard destructor stops and flushes automatically on scope exit.
  }

  const std::string file_content = ReadWholeFile(std::filesystem::path(path + ".1")) + ReadWholeFile(path);
  CHECK(file_content.find("filtered debug record") == std::string::npos);
  CHECK(file_content.find("builder file record 42") != std::string::npos);
  CHECK(file_content.find("builder warn record beef") != std::string::npos);

  // Also test Builder with OutputFd (pipe stream without rotation) and explicit Shutdown().
  int pipe_fds[2]{-1, -1};
  CHECK(::pipe(pipe_fds) == 0);
  auto pipe_guard = Builder().Level(abi::Level::kInfo).OutputFd(pipe_fds[1]).Start();
  CHECK(pipe_guard.has_value());
  zerolog::Info("builder pipe stream ok={}", 99U);
  CHECK(pipe_guard->Shutdown().has_value());
  CHECK(!pipe_guard->Active());
  (void)::close(pipe_fds[1]);

  char buf[512]{};
  const ssize_t n = ::read(pipe_fds[0], buf, sizeof(buf));
  (void)::close(pipe_fds[0]);
  CHECK(n > 0);
  CHECK(std::string_view(buf, static_cast<std::size_t>(n)).find("builder pipe stream ok=99") != std::string_view::npos);
  return true;
}

}  // namespace

int main() {
  if (!TestWorker512KiBStackAndSmallCallerStack()) return EXIT_FAILURE;
  if (!TestFallbackOnAllFiveProactorUnavailableErrors()) return EXIT_FAILURE;
  if (!TestNonFallbackErrorAndLeaseContention()) return EXIT_FAILURE;
  if (!TestIdleWorkerEventDrivenAndImmediateStop()) return EXIT_FAILURE;
  if (!TestBuilderAndLoggerGuardWithFileAndPipeFd()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
