#include <atomic>
#include <cstdint>
#include <thread>
#include <uring/futex2/futex2.hpp>
#include <uring/sys/syscall.hpp>
#include <zerolog/core/doorbell.hpp>

#include "support/check.hpp"

namespace {

using namespace zerolog;
using namespace zerolog::core;

void LevelGateCasIndependenceTest() {
  // 验证并发更新 LevelGateWord 不同位域时互不覆盖独立配置字段
  ResetDoorbellForTesting();

  (void)SetLevel(abi::Level::kDebug);
  (void)FlushOn(abi::Level::kWarn);
  (void)SetHasSinks(true);

  const auto gate1 = LoadLevelGate(std::memory_order_acquire);
  CHECK(gate1.get<^^abi::LevelGateSpec::min_level>() == abi::Level::kDebug);
  CHECK(gate1.get<^^abi::LevelGateSpec::ring_level>() == abi::Level::kWarn);
  CHECK(gate1.get<^^abi::LevelGateSpec::configured_flush_level>() == abi::Level::kWarn);
  CHECK(gate1.test<^^abi::LevelGateSpec::has_sinks>());
  CHECK(!gate1.test<^^abi::LevelGateSpec::deep_park>());

  // 进入 DeepPark 时 ring_level 降为 kTrace，但 configured_flush_level 保持 kWarn
  const auto parked = EnterDeepPark();
  CHECK(parked.test<^^abi::LevelGateSpec::deep_park>());
  CHECK(parked.get<^^abi::LevelGateSpec::ring_level>() == abi::Level::kTrace);
  CHECK(parked.get<^^abi::LevelGateSpec::configured_flush_level>() == abi::Level::kWarn);
  CHECK(parked.get<^^abi::LevelGateSpec::min_level>() == abi::Level::kDebug);

  // DeepPark 期间修改 FlushOn(kError) 更新 configured_flush_level，退出后恢复为 kError
  (void)FlushOn(abi::Level::kError);
  CHECK(LoadLevelGate().get<^^abi::LevelGateSpec::ring_level>() == abi::Level::kTrace);
  const auto unparked = ExitDeepPark();
  CHECK(!unparked.test<^^abi::LevelGateSpec::deep_park>());
  CHECK(unparked.get<^^abi::LevelGateSpec::ring_level>() == abi::Level::kError);
  CHECK(unparked.get<^^abi::LevelGateSpec::configured_flush_level>() == abi::Level::kError);
  CHECK(unparked.get<^^abi::LevelGateSpec::min_level>() == abi::Level::kDebug);
}

void ConcurrentLevelGateStressTest() {
  // 验证多线程并发修改 min_level 与 DeepPark 后目标状态精确保留
  ResetDoorbellForTesting();
  std::atomic<bool> go{false};

  std::thread t1([&]() {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 2000; ++i) {
      (void)SetLevel(abi::Level::kTrace);
      (void)SetLevel(abi::Level::kWarn);
    }
  });

  std::thread t2([&]() {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 2000; ++i) {
      (void)EnterDeepPark();
      (void)ExitDeepPark();
    }
  });

  std::thread t3([&]() {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < 2000; ++i) {
      (void)FlushOn(abi::Level::kInfo);
      (void)FlushOn(abi::Level::kError);
    }
  });

  go.store(true, std::memory_order_release);
  t1.join();
  t2.join();
  t3.join();

  const auto final_gate = LoadLevelGate(std::memory_order_acquire);
  CHECK(final_gate.get<^^abi::LevelGateSpec::min_level>() == abi::Level::kWarn);
  CHECK(!final_gate.test<^^abi::LevelGateSpec::deep_park>());
  CHECK(final_gate.get<^^abi::LevelGateSpec::configured_flush_level>() == abi::Level::kError);
  CHECK(final_gate.get<^^abi::LevelGateSpec::ring_level>() == abi::Level::kError);
}

void DoorbellParkAndFutexWakeTest() {
  // 验证消费者捕获序号后 CAS 进入 parked 并被生产者通过私有 futex2 唤醒
  ResetDoorbellForTesting();

  auto captured = LoadDoorbell();
  CHECK(!captured.test<^^abi::DoorbellSpec::parked>());

  // 若在扫描期间生产者敲响门铃，TryParkDoorbell 必须拒绝进入休眠
  RingDoorbellUrgentCold();
  CHECK(!TryParkDoorbell(captured));
  CHECK(captured.test<^^abi::DoorbellSpec::urgent>());
  CHECK(captured.get<^^abi::DoorbellSpec::ring_seq>() == 1U);

  const auto consumed = ConsumeDoorbellNotification();
  CHECK(consumed.test<^^abi::DoorbellSpec::urgent>());
  CHECK(!LoadDoorbell().test<^^abi::DoorbellSpec::urgent>());

  // 重新捕获并在无并发竞争下成功置位 parked
  captured = LoadDoorbell();
  CHECK(TryParkDoorbell(captured));
  CHECK(captured.test<^^abi::DoorbellSpec::parked>());

  auto ref = DoorbellFutexRef();
  CHECK(ref.has_value());
  CHECK(ref->flags == abi::kDoorbellFutexFlags);

  std::atomic<bool> waiter_entered{false};
  std::atomic<bool> wake_ok{false};

  std::thread waiter([&]() {
    uring::abi::KernelTimespec now{};
    (void)uring::sys::Syscall6(228, 1, uring::sys::Address(&now));
    now.tv_sec += 5;
    const uring::abi::AbsoluteDeadline deadline{now, uring::abi::FutexClock::kMonotonic, 0U};
    waiter_entered.store(true, std::memory_order_release);
    auto res = uring::futex2::Wait(*ref, captured.raw(), deadline);
    if (res && (*res == uring::futex2::WaitOutcome::kWoken || *res == uring::futex2::WaitOutcome::kValueChanged)) {
      wake_ok.store(true, std::memory_order_release);
    }
  });

  while (!waiter_entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  for (int i = 0; i < 1000 && LoadDoorbell().test<^^abi::DoorbellSpec::parked>(); ++i) {
    RingDoorbellCold();
    std::this_thread::yield();
  }

  waiter.join();
  CHECK(wake_ok.load(std::memory_order_acquire));
  CHECK(!LoadDoorbell().test<^^abi::DoorbellSpec::parked>());
}

void DeepParkMembarrierSequenceTest() {
  // 验证 DeepPark 协议中的私有加速内存屏障注册与执行
  ResetDoorbellForTesting();
  auto reg = core::RegisterPrivateExpeditedMembarrier();
  if (reg.has_value()) {
    (void)EnterDeepPark();
    CHECK(core::PrivateExpeditedMembarrier().has_value());
    (void)ExitDeepPark();
  }
}

}  // namespace

int main() {
  LevelGateCasIndependenceTest();
  ConcurrentLevelGateStressTest();
  DoorbellParkAndFutexWakeTest();
  DeepParkMembarrierSequenceTest();
  return 0;
}
