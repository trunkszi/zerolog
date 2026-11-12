#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include "zerolog/sink/wake_timer_fsm.hpp"
#include "zerolog/sink/write_rotate_fsm.hpp"

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) noexcept {
  if (condition) return;
  std::fprintf(stderr, "FAIL %s:%d %s\n", "fsm_test.cc", line, expression);
  ++g_failures;
}

#define CHECK(condition)                  \
  do {                                    \
    if (!(condition)) {                   \
      Check(false, #condition, __LINE__); \
      return;                             \
    }                                     \
  } while (0)

using zerolog::abi::LifecycleState;
using zerolog::abi::SinkFlagsSpec;
using zerolog::abi::SinkFlagsWord;
using zerolog::abi::TimerState;
using zerolog::abi::WakeState;
using zerolog::abi::WriteRotateState;
using zerolog::sink::IoStep;
using zerolog::sink::TimerFsm;
using zerolog::sink::WakeFsm;
using zerolog::sink::WriteRotateFsm;

[[nodiscard]] SinkFlagsWord ReadyFlags() noexcept {
  return SinkFlagsWord{}
      .with<^^SinkFlagsSpec::lifecycle>(LifecycleState::kRunning)
      .with<^^SinkFlagsSpec::io>(WriteRotateState::kReady);
}

[[nodiscard]] WakeState WakeOf(const SinkFlagsWord& flags) noexcept { return flags.get<^^SinkFlagsSpec::wake>(); }
[[nodiscard]] TimerState TimerOf(const SinkFlagsWord& flags) noexcept { return flags.get<^^SinkFlagsSpec::timer>(); }
[[nodiscard]] WriteRotateState IoOf(const SinkFlagsWord& flags) noexcept { return flags.get<^^SinkFlagsSpec::io>(); }

struct TempDir {
  std::array<char, 64> path{};
  explicit TempDir() noexcept {
    std::snprintf(path.data(), path.size(), "/tmp/zl-fsm-XXXXXX");
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

[[nodiscard]] std::span<const std::byte> Bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

[[nodiscard]] bool WriteFile(const char* path, std::string_view text) noexcept {
  const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const auto wrote = ::write(fd, text.data(), text.size());
  ::close(fd);
  return wrote == static_cast<ssize_t>(text.size());
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

void WakeArmCancelReconcileTest() noexcept {
  SinkFlagsWord flags = ReadyFlags();
  const auto idle = WakeFsm::OnWakeCqe(flags, 0);
  CHECK(!idle.has_value());
  CHECK(idle.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(WakeOf(flags) == WakeState::kIdle);
  CHECK(WakeFsm::TryArm(flags).has_value());
  CHECK(WakeOf(flags) == WakeState::kArmed);
  const auto twice = WakeFsm::TryArm(flags);
  CHECK(!twice.has_value());
  CHECK(twice.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(WakeFsm::RequestCancel(flags));
  CHECK(WakeOf(flags) == WakeState::kCanceling);
  CHECK(!WakeFsm::RequestCancel(flags));
  WakeFsm::ReconcileSyncCancel(flags);
  CHECK(WakeOf(flags) == WakeState::kIdle);
  WakeFsm::ReconcileSyncCancel(flags);
  CHECK(WakeOf(flags) == WakeState::kIdle);

  CHECK(WakeFsm::TryArm(flags).has_value());
  const auto woke = WakeFsm::OnWakeCqe(flags, 0);
  CHECK(woke.has_value());
  CHECK(*woke);
  CHECK(WakeOf(flags) == WakeState::kIdle);
  CHECK(WakeFsm::TryArm(flags).has_value());
  const auto again = WakeFsm::OnWakeCqe(flags, -EAGAIN);
  CHECK(again.has_value());
  CHECK(*again);

  CHECK(WakeFsm::TryArm(flags).has_value());
  CHECK(WakeFsm::RequestCancel(flags));
  const auto canceled = WakeFsm::OnWakeCqe(flags, -ECANCELED);
  CHECK(canceled.has_value());
  CHECK(!*canceled);
  CHECK(WakeOf(flags) == WakeState::kIdle);

  CHECK(WakeFsm::TryArm(flags).has_value());
  const auto illegal = WakeFsm::OnWakeCqe(flags, -ECANCELED);
  CHECK(!illegal.has_value());
  CHECK(illegal.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(WakeOf(flags) == WakeState::kIdle);

  CHECK(WakeFsm::TryArm(flags).has_value());
  const auto lost = WakeFsm::OnWakeCqe(flags, -EIO);
  CHECK(!lost.has_value());
  CHECK(lost.error() == zerolog::ErrorCode::kProactorUnavailable);
  CHECK(WakeOf(flags) == WakeState::kIdle);

  flags = flags.with<^^SinkFlagsSpec::lifecycle>(LifecycleState::kStopping);
  const auto stopping = WakeFsm::TryArm(flags);
  CHECK(!stopping.has_value());
  CHECK(stopping.error() == zerolog::ErrorCode::kShuttingDown);
}

void WakeCancelSuppressesRearmTest() noexcept {
  constexpr int results[] = {0, -EAGAIN, -ECANCELED};
  for (const int result : results) {
    SinkFlagsWord flags = ReadyFlags();
    CHECK(WakeFsm::TryArm(flags).has_value());
    CHECK(WakeFsm::RequestCancel(flags));
    CHECK(WakeOf(flags) == WakeState::kCanceling);
    const auto done = WakeFsm::OnWakeCqe(flags, result);
    CHECK(done.has_value());
    CHECK(!*done);
    CHECK(WakeOf(flags) == WakeState::kIdle);
  }
}

void TimerUpdateBenignCqeTest() noexcept {
  TimerFsm timer;
  SinkFlagsWord flags = ReadyFlags();
  CHECK(TimerFsm::TryArm(flags).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);
  const auto early = timer.OnUpdateCqe(flags, 0);
  CHECK(!early.has_value());
  CHECK(early.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());

  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  CHECK(TimerOf(flags) == TimerState::kUpdating);
  CHECK(timer.UpdateInFlight());
  CHECK(!TimerFsm::RequestCancel(flags));
  CHECK(TimerOf(flags) == TimerState::kUpdating);
  CHECK(timer.OnUpdateCqe(flags, 0).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());

  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  CHECK(timer.OnUpdateCqe(flags, -EALREADY).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);

  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  CHECK(timer.OnUpdateCqe(flags, -ENOENT).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());

  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  timer.ReconcileSyncUpdateCancel(flags);
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());

  const auto fired = TimerFsm::OnTimerCqe(flags, -ETIME);
  CHECK(fired.has_value());
  CHECK(*fired);
  CHECK(TimerOf(flags) == TimerState::kIdle);
  const auto orphan = timer.OnUpdateCqe(flags, 0);
  CHECK(!orphan.has_value());
  CHECK(orphan.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(TimerOf(flags) == TimerState::kIdle);
  CHECK(!timer.InFlight(flags));

  CHECK(TimerFsm::TryArm(flags).has_value());
  CHECK(TimerFsm::RequestCancel(flags));
  const auto canceled = TimerFsm::OnTimerCqe(flags, -ECANCELED);
  CHECK(canceled.has_value());
  CHECK(!*canceled);
  CHECK(TimerOf(flags) == TimerState::kIdle);

  CHECK(TimerFsm::TryArm(flags).has_value());
  const auto illegal = TimerFsm::OnTimerCqe(flags, -ECANCELED);
  CHECK(!illegal.has_value());
  CHECK(illegal.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(TimerOf(flags) == TimerState::kIdle);

  CHECK(TimerFsm::TryArm(flags).has_value());
  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  const auto broken = timer.OnUpdateCqe(flags, -EIO);
  CHECK(!broken.has_value());
  CHECK(broken.error() == zerolog::ErrorCode::kWriteFailed);
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());

  flags = flags.with<^^SinkFlagsSpec::lifecycle>(LifecycleState::kClosed);
  const auto stopping = timer.RequestImmediateUpdate(flags);
  CHECK(!stopping.has_value());
  CHECK(stopping.error() == zerolog::ErrorCode::kShuttingDown);
}

void TimerStaleUpdateKeepsArmTest() noexcept {
  TimerFsm timer;
  SinkFlagsWord flags = ReadyFlags();
  CHECK(TimerFsm::TryArm(flags).has_value());
  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  const auto fired = TimerFsm::OnTimerCqe(flags, -ETIME);
  CHECK(fired.has_value());
  CHECK(*fired);
  CHECK(TimerOf(flags) == TimerState::kIdle);
  CHECK(timer.UpdateInFlight());
  CHECK(timer.InFlight(flags));
  CHECK(TimerFsm::TryArm(flags).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(timer.UpdateInFlight());
  CHECK(timer.OnUpdateCqe(flags, -ENOENT).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());
}

void TimerUpdateBusyUntilCqeTest() noexcept {
  TimerFsm timer;
  SinkFlagsWord flags = ReadyFlags();
  CHECK(TimerFsm::TryArm(flags).has_value());
  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  CHECK(TimerFsm::OnTimerCqe(flags, -ETIME).has_value());
  CHECK(TimerFsm::TryArm(flags).has_value());
  const auto busy = timer.RequestImmediateUpdate(flags);
  CHECK(!busy.has_value());
  CHECK(busy.error() == zerolog::ErrorCode::kConsumerBusy);
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(timer.UpdateInFlight());
  CHECK(timer.OnUpdateCqe(flags, 0).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());
  CHECK(timer.RequestImmediateUpdate(flags).has_value());
  CHECK(TimerOf(flags) == TimerState::kUpdating);
  CHECK(timer.OnUpdateCqe(flags, -EALREADY).has_value());
  CHECK(TimerOf(flags) == TimerState::kArmed);
  CHECK(!timer.UpdateInFlight());
}

void ShortWriteBlocksRotateTest() noexcept {
  WriteRotateFsm fsm;
  CHECK(fsm.Bind(3).has_value());
  zerolog::Rotation rotation;
  SinkFlagsWord flags = ReadyFlags();
  const auto began = fsm.BeginWrite(flags, 8);
  CHECK(began.has_value());
  CHECK(began->step == IoStep::kWrite);
  CHECK(began->length == 8);
  CHECK(IoOf(flags) == WriteRotateState::kWriting);
  const auto busy = fsm.BeginRotation(flags, rotation);
  CHECK(!busy.has_value());
  CHECK(busy.error() == zerolog::ErrorCode::kConsumerBusy);

  const auto partial = fsm.OnWriteCqe(flags, 3, 8);
  CHECK(partial.has_value());
  CHECK(partial->step == IoStep::kWrite);
  CHECK(partial->length == 5);
  CHECK(fsm.PendingLength() == 5);
  CHECK(IoOf(flags) == WriteRotateState::kWriting);
  const auto still = fsm.BeginRotation(flags, rotation);
  CHECK(!still.has_value());
  CHECK(still.error() == zerolog::ErrorCode::kConsumerBusy);

  const auto rest = fsm.OnWriteCqe(flags, 5, 5);
  CHECK(rest.has_value());
  CHECK(rest->step == IoStep::kNone);
  CHECK(fsm.PendingLength() == 0);
  CHECK(IoOf(flags) == WriteRotateState::kReady);
  const auto rotate = fsm.BeginRotation(flags, rotation);
  CHECK(rotate.has_value());
  CHECK(rotate->step == IoStep::kUnlinkOldest);

  WriteRotateFsm other;
  CHECK(other.Bind(1).has_value());
  SinkFlagsWord second = ReadyFlags();
  const auto empty = other.BeginWrite(second, 0);
  CHECK(!empty.has_value());
  CHECK(empty.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(other.BeginWrite(second, 4).has_value());
  const auto zero = other.OnWriteCqe(second, 0, 4);
  CHECK(!zero.has_value());
  CHECK(zero.error() == zerolog::ErrorCode::kShortWrite);
  CHECK(other.PendingLength() == 4);
  CHECK(IoOf(second) == WriteRotateState::kReady);
  const auto blocked = other.BeginRotation(second, rotation);
  CHECK(!blocked.has_value());
  CHECK(blocked.error() == zerolog::ErrorCode::kConsumerBusy);
}

void OpenFailAllowsWriteTest() noexcept {
  TempDir dir;
  char base[128]{};
  CHECK(dir.Join(base, sizeof(base), "base"));
  zerolog::Rotation rotation;
  CHECK(rotation.Open(base, zerolog::RotationPolicy{.max_bytes = 64, .max_files = 3}).has_value());
  CHECK(WriteFile(base, "LIVE"));
  WriteRotateFsm fsm;
  CHECK(fsm.Bind(3).has_value());
  SinkFlagsWord flags = ReadyFlags();
  auto step = fsm.BeginRotation(flags, rotation);
  CHECK(step.has_value());
  CHECK(step->step == IoStep::kUnlinkOldest);
  step = fsm.OnUnlinkCqe(flags, rotation, 0);
  CHECK(step.has_value());
  CHECK(step->step == IoStep::kRename);
  CHECK(step->rename_slot == 2);
  step = fsm.OnRenameCqe(flags, rotation, 0);
  CHECK(step.has_value());
  CHECK(step->rename_slot == 1);
  step = fsm.OnRenameCqe(flags, rotation, 0);
  CHECK(step.has_value());
  CHECK(step->rename_slot == 0);
  step = fsm.OnRenameCqe(flags, rotation, 0);
  CHECK(step.has_value());
  CHECK(step->step == IoStep::kOpenNew);
  CHECK(rotation.ReopenPending());
  const auto failed = fsm.OnOpenCqe(flags, rotation, -EIO);
  CHECK(!failed.has_value());
  CHECK(failed.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(rotation.ReopenPending());
  CHECK(IoOf(flags) == WriteRotateState::kReady);
  CHECK(flags.test<^^SinkFlagsSpec::rotation_retry>());
  CHECK(fsm.RotationBackoffMs() == 1);

  const auto write = fsm.BeginWrite(flags, 4);
  CHECK(write.has_value());
  CHECK(write->step == IoStep::kWrite);
  const auto progress = rotation.WriteSync(Bytes("MORE"));
  CHECK(progress.error == zerolog::ErrorCode::kSuccess);
  CHECK(progress.written == 4);
  CHECK(ReadEquals(base, "LIVEMORE"));
  const auto done = fsm.OnWriteCqe(flags, 4, 4);
  CHECK(done.has_value());
  CHECK(done->step == IoStep::kNone);
  const auto waiting = fsm.BeginRotation(flags, rotation);
  CHECK(waiting.has_value());
  CHECK(waiting->step == IoStep::kWaitBackoff);
  CHECK(waiting->backoff_ms == 1);
  const auto resumed = fsm.OnBackoffElapsed(flags, rotation, false);
  CHECK(resumed.has_value());
  CHECK(resumed->step == IoStep::kOpenNew);
  CHECK(IoOf(flags) == WriteRotateState::kOpening);
  CHECK(!flags.test<^^SinkFlagsSpec::rotation_retry>());
}

void BackoffDoublesUntilCapTest() noexcept {
  constexpr std::uint32_t delays[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1000, 1000};
  WriteRotateFsm fsm;
  CHECK(!fsm.Bind(0).has_value());
  CHECK(!fsm.Bind(16).has_value());
  CHECK(fsm.Bind(3).has_value());
  zerolog::Rotation rotation;
  SinkFlagsWord flags = ReadyFlags();
  const auto stray = fsm.OnBackoffElapsed(flags, rotation, true);
  CHECK(!stray.has_value());
  CHECK(stray.error() == zerolog::ErrorCode::kInvalidArgument);
  CHECK(fsm.BeginWrite(flags, 4).has_value());
  for (const std::uint32_t delay : delays) {
    const auto failed = fsm.OnWriteCqe(flags, -EIO, 4);
    CHECK(!failed.has_value());
    CHECK(failed.error() == zerolog::ErrorCode::kWriteFailed);
    CHECK(fsm.WriteBackoffMs() == delay);
    CHECK(flags.test<^^SinkFlagsSpec::write_retry>());
    const auto held = fsm.BeginWrite(flags, 4);
    CHECK(held.has_value());
    CHECK(held->step == IoStep::kWaitBackoff);
    CHECK(held->backoff_ms == delay);
    const auto resumed = fsm.OnBackoffElapsed(flags, rotation, true);
    CHECK(resumed.has_value());
    CHECK(resumed->step == IoStep::kWrite);
    CHECK(resumed->length == 4);
    CHECK(IoOf(flags) == WriteRotateState::kWriting);
  }
  const auto mismatch = fsm.OnWriteCqe(flags, 4, 3);
  CHECK(!mismatch.has_value());
  CHECK(mismatch.error() == zerolog::ErrorCode::kInvalidArgument);
  const auto over = fsm.OnWriteCqe(flags, 5, 4);
  CHECK(!over.has_value());
  CHECK(over.error() == zerolog::ErrorCode::kWriteFailed);
  CHECK(fsm.PendingLength() == 0);
  const auto idle = fsm.OnWriteCqe(flags, 4, 4);
  CHECK(!idle.has_value());
  CHECK(idle.error() == zerolog::ErrorCode::kInvalidArgument);
}

void FsmHandoffResumesSlotTest() noexcept {
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
  WriteRotateFsm fsm;
  CHECK(fsm.Bind(3).has_value());
  SinkFlagsWord flags = ReadyFlags();
  auto step = fsm.BeginRotation(flags, rotation);
  CHECK(step.has_value());
  CHECK(step->step == IoStep::kUnlinkOldest);
  step = fsm.OnUnlinkCqe(flags, rotation, 0);
  CHECK(step.has_value());
  CHECK(step->rename_slot == 2);
  step = fsm.OnRenameCqe(flags, rotation, 0);
  CHECK(step.has_value());
  CHECK(step->rename_slot == 1);
  const auto blocked = fsm.OnRenameCqe(flags, rotation, -EPERM);
  CHECK(!blocked.has_value());
  CHECK(blocked.error() == zerolog::ErrorCode::kRotateFailed);
  CHECK(rotation.OldestUnlinked());
  CHECK(rotation.NextRenameSlot() == 1);
  CHECK(!rotation.ReopenPending());
  CHECK(IoOf(flags) == WriteRotateState::kReady);
  CHECK(ReadEquals(base, "LIVE"));
  CHECK(ReadEquals(base1, "ONE"));
  CHECK(ReadEquals(base2, "TWO"));
  CHECK(ReadEquals(base3, "OLD"));
  const auto recovered = rotation.RotateSync();
  CHECK(recovered.has_value());
  CHECK(ReadEquals(base, ""));
  CHECK(ReadEquals(base1, "LIVE"));
  CHECK(ReadEquals(base2, "ONE"));
  CHECK(ReadEquals(base3, "OLD"));
}

void SharedDrainFlagsTest() noexcept {
  using namespace zerolog;
  CHECK(!sink::detail::DrainHasMore(abi::DrainFlagsWord{}));
  CHECK(sink::detail::DrainHasMore(abi::DrainFlagsWord{}.with<^^abi::DrainFlagsSpec::budget_exhausted>(true)));
  CHECK(sink::detail::DrainHasMore(abi::DrainFlagsWord{}.with<^^abi::DrainFlagsSpec::more_pending>(true)));
  CHECK(sink::detail::DrainHasMore(abi::DrainFlagsWord{}.with<^^abi::DrainFlagsSpec::backpressure>(true)));
}

void SharedBackoffBoundaryTest() noexcept {
  std::uint8_t shift = 0;
  for (const auto expected : std::array{1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U, 1000U, 1000U}) {
    CHECK(zerolog::sink::detail::BumpBackoffMs(shift) == expected);
  }
  CHECK(shift == 10U);
  shift = 255U;
  CHECK(zerolog::sink::detail::BumpBackoffMs(shift) == 1000U && shift == 255U);
}

}  // namespace

int main() {
  SharedDrainFlagsTest();
  SharedBackoffBoundaryTest();
  WakeArmCancelReconcileTest();
  WakeCancelSuppressesRearmTest();
  TimerUpdateBenignCqeTest();
  TimerStaleUpdateKeepsArmTest();
  TimerUpdateBusyUntilCqeTest();
  ShortWriteBlocksRotateTest();
  OpenFailAllowsWriteTest();
  BackoffDoublesUntilCapTest();
  FsmHandoffResumesSlotTest();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d fsm checks failed\n", g_failures);
    return 1;
  }
  std::printf("fsm_test ok\n");
  return 0;
}
