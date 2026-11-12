#include "zerolog/sink/proactor_sink.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <uring/core/dispatcher.hpp>
#include <uring/core/proactor.hpp>
#include <zerolog/core/consumer_core.hpp>
#include <zerolog/core/doorbell.hpp>
#include <zerolog/core/lease.hpp>
#include <zerolog/core/producer.hpp>
#include <zerolog/core/registry.hpp>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) noexcept {
  if (condition) return;
  std::fprintf(stderr, "FAIL %s:%d %s\n", "proactor_sink_test.cc", line, expression);
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
    std::snprintf(path.data(), path.size(), "/tmp/zl-psink-XXXXXX");
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

[[nodiscard]] std::string ReadFileString(const char* path) noexcept {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  std::string out;
  std::array<char, 4096> buf{};
  for (;;) {
    const auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) break;
    out.append(buf.data(), static_cast<std::size_t>(n));
  }
  ::close(fd);
  return out;
}

void ResetGlobals() noexcept {
  zerolog::core::ResetDoorbellForTesting();
  zerolog::core::ResetLeaseStateForTesting();
  zerolog::core::SetLevel(zerolog::abi::Level::kTrace);
  if (zerolog::core::t_buffer != nullptr) {
    for (;;) {
      const auto view = zerolog::core::t_buffer->queue.Front();
      if (!view.Valid()) break;
      zerolog::core::t_buffer->queue.Pop(view.header.get<^^zerolog::abi::RecordHeaderSpec::blocks>());
    }
  }
}

void ScriptedLifecycleAndDeferredCancelTest() noexcept {
  ResetGlobals();
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));

  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 4096U, .max_files = 3U}).has_value());

  auto consumer = zerolog::core::ConsumerCore::Create();
  CHECK(consumer.has_value());

  auto proactor = uring::Proactor::Builder().Entries(8U).CqEntries(16U).BuildScripted();
  CHECK(proactor.has_value());

  using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 1>;
  Sink sink{*proactor, *consumer, rotation};
  uring::SingleDispatcher<Sink> dispatcher{sink};
  proactor->BindDispatcher(dispatcher);

  // Fill the scripted SQ ring so Start()'s wake and timer operations are staged into the deferred queue.
  proactor->Backend().SetConsumeSqOnSubmit(false);
  while (proactor->Backend().Sq().FreeSlots() != 0U) {
    auto entry = proactor->Backend().Sq().TryReserve64();
    if (!entry) break;
    uring::ops::PrepareNop().Encode(**entry);
  }

  CHECK(sink.Start().has_value());
  CHECK(!sink.Quiescent());
  CHECK(proactor->DeferredCount() == 2U);
  CHECK(sink.Flags().get<^^zerolog::abi::SinkFlagsSpec::wake>() == zerolog::abi::WakeState::kArmed);
  CHECK(sink.Flags().get<^^zerolog::abi::SinkFlagsSpec::timer>() == zerolog::abi::TimerState::kArmed);

  // Deferred cancel removes the SQE from the deferred queue and keeps the intent until Tick
  // delivers the synthetic completion that retires wake and timer.
  CHECK(sink.RequestStop(*proactor).has_value());
  CHECK(proactor->DeferredCount() == 0U);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.Quiescent());

  // Log a record after stop and flush synchronously via ShutdownFlushSync.
  zerolog::Info("deferred_cancel_ok={}", 42U);
  CHECK(sink.ShutdownFlushSync().has_value());
  const auto content = ReadFileString(base);
  CHECK(content.find("deferred_cancel_ok=42") != std::string::npos);

  proactor->Backend().SetConsumeSqOnSubmit(true);
  proactor->Backend().AdvanceSqHead(proactor->Backend().Sq().UnflushedCount());
  CHECK(proactor->ShutdownWith(dispatcher).has_value());
}

void ShortWriteRearmsSuffixBeforeRotationTest() noexcept {
  ResetGlobals();
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));

  zerolog::Rotation rotation;
  // Set max_bytes = 16 so even a partial write exceeds max_bytes.
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 16U, .max_files = 2U}).has_value());

  auto consumer = zerolog::core::ConsumerCore::Create();
  CHECK(consumer.has_value());

  auto proactor = uring::Proactor::Builder().Entries(16U).CqEntries(32U).BuildScripted();
  CHECK(proactor.has_value());

  using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 2>;
  Sink sink{*proactor, *consumer, rotation};
  uring::SingleDispatcher<Sink> dispatcher{sink};
  proactor->BindDispatcher(dispatcher);

  zerolog::Info("short_write_payload={}", 123456U);
  CHECK(sink.Start().has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kWrite);

  const auto total_len = sink.IoFsm().PendingLength();
  CHECK(total_len > 20U);
  const std::uint32_t first_part = 20U;
  const std::uint32_t remaining = total_len - first_part;

  // Script a short write of 20 bytes (which already exceeds max_bytes = 16).
  proactor->Backend().ScriptCqe(sink.IoKey().token, static_cast<std::int32_t>(first_part));
  CHECK(proactor->Tick(dispatcher).has_value());

  // Must stay in kWriting and re-arm WRITE for the remaining suffix BEFORE starting any rotation!
  CHECK(rotation.WrittenBytes() == first_part);
  CHECK(rotation.NeedsRotation());
  CHECK(sink.Flags().get<^^zerolog::abi::SinkFlagsSpec::io>() == zerolog::abi::WriteRotateState::kWriting);
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kWrite);
  CHECK(sink.IoFsm().PendingLength() == remaining);
  CHECK(consumer->Output().FrozenBytes() == remaining);

  // Now complete the remaining suffix.
  proactor->Backend().ScriptCqe(sink.IoKey().token, static_cast<std::int32_t>(remaining));
  CHECK(proactor->Tick(dispatcher).has_value());

  // Only after the full slab suffix finishes does rotation begin at kUnlinkOldest.
  CHECK(consumer->Output().FrozenBytes() == 0U);
  CHECK(sink.IoFsm().PendingLength() == 0U);
  CHECK(sink.Flags().get<^^zerolog::abi::SinkFlagsSpec::io>() == zerolog::abi::WriteRotateState::kRenaming);
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kUnlinkOldest);

  // Stop while rotation is in flight and let the unlink CQE retire to hand off to ShutdownFlushSync.
  CHECK(sink.RequestStop().has_value());
  proactor->Backend().ScriptCqe(sink.WakeKey().token, -ECANCELED);
  proactor->Backend().ScriptCqe(sink.TimerKey().token, -ECANCELED);
  proactor->Backend().ScriptCqe(sink.IoKey().token, 0);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.Quiescent());
  CHECK(proactor->ShutdownWith(dispatcher).has_value());
  CHECK(sink.ShutdownFlushSync().has_value());
}

void OpenOnlyRotationRecoveryTest() noexcept {
  ResetGlobals();
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));

  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 8U, .max_files = 2U}).has_value());

  auto consumer = zerolog::core::ConsumerCore::Create();
  CHECK(consumer.has_value());

  auto proactor = uring::Proactor::Builder().Entries(16U).CqEntries(32U).BuildScripted();
  CHECK(proactor.has_value());

  using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 3>;
  Sink sink{*proactor, *consumer, rotation};
  uring::SingleDispatcher<Sink> dispatcher{sink};
  proactor->BindDispatcher(dispatcher);

  // Seed initial content and trigger rotation.
  const std::string_view initial = "INITIAL_LOG\n";
  const auto wrote_init = rotation.WriteSync(initial);
  CHECK(wrote_init.error == zerolog::ErrorCode::kSuccess);
  CHECK(rotation.NeedsRotation());

  CHECK(sink.Start().has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kUnlinkOldest);

  // Step 1: Unlink oldest (base.2) succeeds.
  proactor->Backend().ScriptCqe(sink.IoKey().token, -ENOENT);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kRename);

  // Step 2: Rename base.1 -> base.2 (-ENOENT).
  proactor->Backend().ScriptCqe(sink.IoKey().token, -ENOENT);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kRename);

  // Step 3: Perform actual rename of base -> base.1 on disk and script CQE 0.
  CHECK(::rename(base, base1) == 0);
  proactor->Backend().ScriptCqe(sink.IoKey().token, 0);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kOpenNew);
  CHECK(rotation.ReopenPending());

  // Step 4: OPENAT fails with -EMFILE.
  proactor->Backend().ScriptCqe(sink.IoKey().token, -EMFILE);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.LastError() == zerolog::ErrorCode::kRotateFailed);
  CHECK(rotation.ReopenPending());
  CHECK(sink.Flags().test<^^zerolog::abi::SinkFlagsSpec::rotation_retry>());
  CHECK(sink.Flags().get<^^zerolog::abi::SinkFlagsSpec::io>() == zerolog::abi::WriteRotateState::kReady);

  // Step 5: During rotation backoff, new logs must still be written to the retained fd (now base.1).
  zerolog::Info("during_rotation_backoff={}", 7U);
  proactor->Backend().ScriptCqe(sink.WakeKey().token, 0);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kWrite);

  // Perform the actual write to rotation.Fd() and complete the write CQE.
  const auto pending_len = sink.IoFsm().PendingLength();
  CHECK(pending_len > 0U);
  proactor->Backend().ScriptCqe(sink.IoKey().token, static_cast<std::int32_t>(pending_len));
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.Flags().get<^^zerolog::abi::SinkFlagsSpec::io>() == zerolog::abi::WriteRotateState::kReady);

  // Step 6: Timer expires (-ETIME), elapsing the rotation backoff window.
  // Must retry ONLY OPENAT (kOpenNew), never re-running Unlink or Rename!
  proactor->Backend().ScriptCqe(sink.TimerKey().token, -ETIME);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kOpenNew);

  const int new_fd = ::open(base, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  CHECK(new_fd >= 0);
  proactor->Backend().ScriptCqe(sink.IoKey().token, new_fd);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(!rotation.ReopenPending());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kCloseOld);

  // Complete close of old fd.
  proactor->Backend().ScriptCqe(sink.IoKey().token, 0);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.Flags().get<^^zerolog::abi::SinkFlagsSpec::io>() == zerolog::abi::WriteRotateState::kReady);
  CHECK(!sink.Flags().test<^^zerolog::abi::SinkFlagsSpec::rotation_retry>());

  CHECK(sink.RequestStop().has_value());
  proactor->Backend().ScriptCqe(sink.WakeKey().token, -ECANCELED);
  proactor->Backend().ScriptCqe(sink.TimerKey().token, -ECANCELED);
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.Quiescent());
  CHECK(proactor->ShutdownWith(dispatcher).has_value());
  CHECK(sink.ShutdownFlushSync().has_value());
}

void Backpressure16mibBudgetCapTest() noexcept {
  ResetGlobals();
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));

  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 64U * 1024U * 1024U, .max_files = 2U}).has_value());

  auto consumer = zerolog::core::ConsumerCore::Create();
  CHECK(consumer.has_value());

  auto proactor = uring::Proactor::Builder().Entries(16U).CqEntries(32U).BuildScripted();
  CHECK(proactor.has_value());

  using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 4>;
  Sink sink{*proactor, *consumer, rotation,
            zerolog::SinkConfig{.drain_budget = {65536U, 16U * 1024U * 1024U}, .idle_timeout_ns = 1'000'000ULL}};
  uring::SingleDispatcher<Sink> dispatcher{sink};
  proactor->BindDispatcher(dispatcher);

  // Log first batch and start sink so one WRITE is in flight (holding frozen_bytes > 0).
  std::array<char, 4096> chunk{};
  chunk.fill('X');
  const std::string_view payload{chunk.data(), chunk.size()};
  zerolog::Info("{}", payload);
  CHECK(sink.Start().has_value());
  CHECK(sink.InFlightIoStep() == zerolog::sink::IoStep::kWrite);
  const auto frozen_bytes = consumer->Output().FrozenBytes();
  CHECK(frozen_bytes > 4096U);

  // While the first WRITE remains in flight (simulating a stalled pipe/disk), flood > 16 MiB into output.
  for (int batch = 0; batch < 12; ++batch) {
    for (int i = 0; i < 380; ++i) {
      const auto err = zerolog::TryInfo("{}", payload);
      if (err == zerolog::ErrorCode::kQueueFull) break;
      CHECK(err == zerolog::ErrorCode::kSuccess);
    }
    proactor->Backend().ScriptCqe(sink.WakeKey().token, 0);
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(sink.TotalUnflushedBytes() <= zerolog::format::kMaxPendingBytes);
    CHECK(consumer->Output().FrozenBytes() == frozen_bytes);
  }

  // TotalUnflushedBytes must be tightly near 16 MiB and strictly <= 16 MiB, with retained head in ScratchHeap.
  CHECK(sink.TotalUnflushedBytes() <= zerolog::format::kMaxPendingBytes);
  CHECK(sink.TotalUnflushedBytes() + 8192U >= zerolog::format::kMaxPendingBytes);
  CHECK(consumer->RetainedHeads() > 0U);

  // Complete the in-flight WRITE and verify backpressured records drain cleanly.
  proactor->Backend().ScriptCqe(sink.IoKey().token, static_cast<std::int32_t>(frozen_bytes));
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.TotalUnflushedBytes() <= zerolog::format::kMaxPendingBytes);

  CHECK(sink.RequestStop().has_value());
  if (sink.WakeKey().token != uring::kNullToken) {
    proactor->Backend().ScriptCqe(sink.WakeKey().token, -ECANCELED);
  }
  if (sink.TimerKey().token != uring::kNullToken) {
    proactor->Backend().ScriptCqe(sink.TimerKey().token, -ECANCELED);
  }
  if (sink.TimerUpdateKey().token != uring::kNullToken) {
    proactor->Backend().ScriptCqe(sink.TimerUpdateKey().token, -ECANCELED);
  }
  if (sink.IoKey().token != uring::kNullToken) {
    proactor->Backend().ScriptCqe(sink.IoKey().token, static_cast<std::int32_t>(sink.IoFsm().PendingLength()));
  }
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.Quiescent());
  CHECK(proactor->ShutdownWith(dispatcher).has_value());
  CHECK(sink.ShutdownFlushSync().has_value());
  CHECK(sink.TotalUnflushedBytes() == 0U);
}

void LiveProactorSinkRotationAndShutdownTest() noexcept {
  ResetGlobals();
  TempDir dir;
  char base[128]{};
  char base1[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  CHECK(dir.Join(base1, sizeof(base1), "base.1"));

  auto built = uring::Proactor::Builder().Entries(64U).Build();
  if (!built) {
    const auto err = zerolog::FromUring(built.error());
    CHECK(err == zerolog::ErrorCode::kProactorUnavailable);
    return;
  }

  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 256U, .max_files = 2U}).has_value());

  auto consumer = zerolog::core::ConsumerCore::Create();
  CHECK(consumer.has_value());

  using Sink = zerolog::ProactorLogSink<uring::Proactor, 1>;
  Sink sink{*built, *consumer, rotation};
  uring::SingleDispatcher<Sink> dispatcher{sink};
  built->BindDispatcher(dispatcher);

  CHECK(sink.Start().has_value());

  for (std::uint32_t i = 0; i < 12U; ++i) {
    zerolog::Info("live_proactor_record={}", i);
  }
  zerolog::core::RingDoorbellUrgentCold();

  for (int step = 0; step < 32; ++step) {
    [[maybe_unused]] const auto waited = built->WaitCompletions(1U, std::chrono::milliseconds(2), dispatcher);
    if (consumer->Output().TotalUnflushedBytes() == 0U && !rotation.OldestUnlinked() && !rotation.ReopenPending()) {
      break;
    }
  }

  CHECK(sink.RequestStop().has_value());
  for (int spin = 0; spin < 256 && !sink.Quiescent(); ++spin) {
    [[maybe_unused]] const auto waited = built->WaitCompletions(1U, std::chrono::milliseconds(2), dispatcher);
  }
  CHECK(sink.Quiescent());
  CHECK(built->ShutdownWith(dispatcher).has_value());
  CHECK(sink.ShutdownFlushSync().has_value());

  const std::string combined = ReadFileString(base) + ReadFileString(base1);
  CHECK(combined.find("live_proactor_record=0") != std::string::npos);
  CHECK(combined.find("live_proactor_record=11") != std::string::npos);
}

int g_psink_fail_rename_errno = 0;

struct ShutdownFaultIo {
  [[nodiscard]] static std::int64_t Write(std::int32_t fd, const std::byte* data, std::uint32_t length) noexcept {
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
    if (g_psink_fail_rename_errno != 0) {
      return -static_cast<std::int64_t>(g_psink_fail_rename_errno);
    }
    return zerolog::sink::PosixIo::RenameAt(olddirfd, oldpath, newdirfd, newpath, flags);
  }
};

void DestructorPanicsWhenInFlightTest() noexcept {
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    // Redirect stderr in the child to keep test output clean when Panic writes its message.
    const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull >= 0) {
      (void)::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ResetGlobals();
    TempDir dir;
    char base[128]{};
    if (!dir.Join(base, sizeof(base), "base")) _exit(0);
    zerolog::Rotation rotation;
    if (!rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 4096U, .max_files = 2U}).has_value()) _exit(0);
    auto consumer = zerolog::core::ConsumerCore::Create();
    if (!consumer.has_value()) _exit(0);
    auto proactor = uring::Proactor::Builder().Entries(8U).CqEntries(16U).BuildScripted();
    if (!proactor.has_value()) _exit(0);
    {
      using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 5>;
      Sink sink{*proactor, *consumer, rotation};
      uring::SingleDispatcher<Sink> dispatcher{sink};
      proactor->BindDispatcher(dispatcher);
      if (!sink.Start().has_value()) _exit(0);
      // Destroy sink without RequestStop / draining while wake and timer are in flight -> must Panic!
    }
    _exit(0);
  }
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0));
}

void ShutdownFlushWritesBufferedLogsOnRotationFailureTest() noexcept {
  ResetGlobals();
  g_psink_fail_rename_errno = 0;
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));

  using FaultRotation = zerolog::sink::BasicRotation<ShutdownFaultIo>;
  FaultRotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 16U, .max_files = 2U}).has_value());

  auto consumer = zerolog::core::ConsumerCore::Create();
  CHECK(consumer.has_value());

  auto proactor = uring::Proactor::Builder().Entries(16U).CqEntries(32U).BuildScripted();
  CHECK(proactor.has_value());

  using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 6, FaultRotation>;
  Sink sink{*proactor, *consumer, rotation};
  uring::SingleDispatcher<Sink> dispatcher{sink};
  proactor->BindDispatcher(dispatcher);

  // Fill SQ so Start() stages wake and timer in the deferred queue. RequestStop then needs one Tick
  // before the synthetic completions make the sink quiescent.
  proactor->Backend().SetConsumeSqOnSubmit(false);
  while (proactor->Backend().Sq().FreeSlots() != 0U) {
    auto entry = proactor->Backend().Sq().TryReserve64();
    if (!entry) break;
    uring::ops::PrepareNop().Encode(**entry);
  }
  CHECK(sink.Start().has_value());
  CHECK(sink.RequestStop(*proactor).has_value());
  CHECK(proactor->Tick(dispatcher).has_value());
  CHECK(sink.Quiescent());

  // Emit records that cross max_bytes=16 and inject rotation failure during ShutdownFlushSync.
  g_psink_fail_rename_errno = EACCES;
  zerolog::Info("shutdown_rot_fail_rec_1={}", 111U);
  zerolog::Info("shutdown_rot_fail_rec_2={}", 222U);

  const auto flushed = sink.ShutdownFlushSync();
  g_psink_fail_rename_errno = 0;
  CHECK(!flushed.has_value());
  CHECK(flushed.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(sink.TotalUnflushedBytes() == 0U);

  const std::string content = ReadFileString(base);
  CHECK(content.find("shutdown_rot_fail_rec_1=111") != std::string::npos);
  CHECK(content.find("shutdown_rot_fail_rec_2=222") != std::string::npos);

  proactor->Backend().SetConsumeSqOnSubmit(true);
  proactor->Backend().AdvanceSqHead(proactor->Backend().Sq().UnflushedCount());
  CHECK(proactor->ShutdownWith(dispatcher).has_value());
}

void AdaptiveTimerDefaultAndDeepParkEscalationTest() noexcept {
  ResetGlobals();
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));

  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 64U * 1024U, .max_files = 2U}).has_value());

  auto consumer = zerolog::core::ConsumerCore::Create();
  CHECK(consumer.has_value());

  // Part 1: Default SinkConfig{} has manage_deep_park == 0 (AdaptiveTimer).
  // Even across multiple empty timer ticks and sparse bursts, DeepPark is never entered.
  {
    auto proactor = uring::Proactor::Builder().Entries(16U).CqEntries(32U).BuildScripted();
    CHECK(proactor.has_value());
    using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 7>;
    Sink sink{*proactor, *consumer, rotation};
    uring::SingleDispatcher<Sink> dispatcher{sink};
    proactor->BindDispatcher(dispatcher);
    CHECK(sink.Start().has_value());
    CHECK(sink.DeepParkEntries() == 0U);
    CHECK(!zerolog::core::LoadLevelGate().test<^^zerolog::abi::LevelGateSpec::deep_park>());

    for (int tick = 0; tick < 5; ++tick) {
      proactor->Backend().ScriptCqe(sink.TimerKey().token, -ETIME);
      CHECK(proactor->Tick(dispatcher).has_value());
    }
    CHECK(sink.DeepParkEntries() == 0U);
    CHECK(!zerolog::core::LoadLevelGate().test<^^zerolog::abi::LevelGateSpec::deep_park>());

    CHECK(sink.RequestStop().has_value());
    proactor->Backend().ScriptCqe(sink.WakeKey().token, -ECANCELED);
    proactor->Backend().ScriptCqe(sink.TimerKey().token, -ECANCELED);
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(sink.Quiescent());
    CHECK(proactor->ShutdownWith(dispatcher).has_value());
  }

  // Part 2: manage_deep_park = 2 enters DeepPark only after 2 consecutive empty idle-timer expirations,
  // and never on wake re-arms or sparse bursts that arrive before 2 empty timer ticks.
  {
    auto proactor = uring::Proactor::Builder().Entries(16U).CqEntries(32U).BuildScripted();
    CHECK(proactor.has_value());
    using Sink = zerolog::ProactorLogSink<uring::ScriptedProactor, 8>;
    Sink sink{
        *proactor, *consumer, rotation,
        zerolog::SinkConfig{.drain_budget = {64U, 1024U * 1024U}, .idle_timeout_ns = 1'000'000ULL, .manage_deep_park = 2U}};
    uring::SingleDispatcher<Sink> dispatcher{sink};
    proactor->BindDispatcher(dispatcher);
    CHECK(sink.Start().has_value());
    CHECK(sink.DeepParkEntries() == 0U);

    // 1st empty timer tick -> still below threshold (1 < 2), not in DeepPark.
    proactor->Backend().ScriptCqe(sink.TimerKey().token, -ETIME);
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(sink.DeepParkEntries() == 0U);
    CHECK(!zerolog::core::LoadLevelGate().test<^^zerolog::abi::LevelGateSpec::deep_park>());

    // Sparse burst arrives on timer tick -> resets empty_idle_ticks_ to 0, still 0 DeepPark entries.
    zerolog::Info("burst_before_deep_park={}", 1U);
    proactor->Backend().ScriptCqe(sink.TimerKey().token, -ETIME);
    CHECK(proactor->Tick(dispatcher).has_value());
    proactor->Backend().ScriptCqe(sink.IoKey().token, static_cast<std::int32_t>(sink.IoFsm().PendingLength()));
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(sink.DeepParkEntries() == 0U);

    // Now 2 consecutive empty timer ticks -> enters DeepPark on the 2nd empty tick!
    proactor->Backend().ScriptCqe(sink.TimerKey().token, -ETIME);
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(sink.DeepParkEntries() == 0U);

    proactor->Backend().ScriptCqe(sink.TimerKey().token, -ETIME);
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(sink.DeepParkEntries() == 1U);
    CHECK(zerolog::core::LoadLevelGate().test<^^zerolog::abi::LevelGateSpec::deep_park>());

    // Next record wakes the doorbell -> immediately exits DeepPark and does not re-enter on ArmWake!
    zerolog::Info("wake_from_deep_park={}", 2U);
    proactor->Backend().ScriptCqe(sink.WakeKey().token, 0);
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(!zerolog::core::LoadLevelGate().test<^^zerolog::abi::LevelGateSpec::deep_park>());
    CHECK(sink.DeepParkEntries() == 1U);
    proactor->Backend().ScriptCqe(sink.IoKey().token, static_cast<std::int32_t>(sink.IoFsm().PendingLength()));
    CHECK(proactor->Tick(dispatcher).has_value());

    CHECK(sink.RequestStop().has_value());
    proactor->Backend().ScriptCqe(sink.WakeKey().token, -ECANCELED);
    proactor->Backend().ScriptCqe(sink.TimerKey().token, -ECANCELED);
    CHECK(proactor->Tick(dispatcher).has_value());
    CHECK(sink.Quiescent());
    CHECK(proactor->ShutdownWith(dispatcher).has_value());
    CHECK(sink.ShutdownFlushSync().has_value());
  }
}

}  // namespace

int main() {
  ScriptedLifecycleAndDeferredCancelTest();
  ShortWriteRearmsSuffixBeforeRotationTest();
  OpenOnlyRotationRecoveryTest();
  Backpressure16mibBudgetCapTest();
  LiveProactorSinkRotationAndShutdownTest();
  DestructorPanicsWhenInFlightTest();
  ShutdownFlushWritesBufferedLogsOnRotationFailureTest();
  AdaptiveTimerDefaultAndDeepParkEscalationTest();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d proactor_sink_test checks failed\n", g_failures);
    return 1;
  }
  std::printf("proactor_sink_test ok\n");
  return 0;
}
