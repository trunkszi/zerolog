#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>
#include <zerolog/core/registry.hpp>
#include <zerolog/core/thread_buffer.hpp>

#include "support/check.hpp"

namespace {

using namespace zerolog;
using namespace zerolog::core;

std::atomic<std::uint32_t> g_callback_hits{0U};

void OnQueueFullTestCallback(std::uint32_t slot, std::uint32_t blocks, abi::Level level) noexcept {
  (void)slot;
  (void)blocks;
  (void)level;
  g_callback_hits.fetch_add(1U, std::memory_order_relaxed);
  // 验证回调重入保护：在回调内部再次触发 ReserveSlow 不会递归调用回调
  if (t_buffer != nullptr) {
    (void)t_buffer->ReserveSlow(abi::kMaxRecordBlocks, abi::Level::kWarn);
  }
}

void SlotExhaustionAndReclaimTest() {
  // 验证 64 槽满载、第 65 槽拒绝及孤儿槽排空后 epoch+1 回收复用
  RetireCurrentThread();
  GlobalRegistry().ResetForTesting();
  ResetDoorbellForTesting();

  std::array<std::uint32_t, kMaxThreads> claimed_slots{};
  for (std::uint32_t i = 0; i < kMaxThreads; ++i) {
    std::uint32_t slot = 999U;
    ThreadBuffer* buf = GlobalRegistry().RegisterSlot(&slot);
    CHECK(buf != nullptr);
    CHECK(slot == i);
    claimed_slots[i] = slot;
  }
  CHECK(GlobalRegistry().ActiveSlotsBitmap().is_full());

  // 第 65 次注册必须返回 nullptr / ErrorCode::kTooManyThreads 并累加 RejectedThreads 计数
  auto over_res = GlobalRegistry().TryRegisterSlot();
  CHECK(!over_res.has_value() && over_res.error() == ErrorCode::kTooManyThreads);
  CHECK(GlobalRegistry().RejectedThreads() == 1U);

  // 在槽 0 中写入一条记录后将槽 0 退休为 kOrphaned
  ThreadBuffer* buf0 = GlobalRegistry().GetSlotBuffer(0U);
  CHECK(buf0 != nullptr);
  std::byte* rec = buf0->TryReserve(4U);
  CHECK(rec != nullptr);
  SpscVarQueue::Publish(rec, abi::RecordHeaderWord{}
                                 .with<^^abi::RecordHeaderSpec::blocks>(4U)
                                 .with<^^abi::RecordHeaderSpec::level>(abi::Level::kInfo)
                                 .with<^^abi::RecordHeaderSpec::payload_bytes>(0U));

  GlobalRegistry().RetireSlot(0U);
  CHECK(GlobalRegistry().GetSlotEpochWord(0U).get<^^abi::SlotEpochSpec::state>() == abi::SlotState::kOrphaned);

  // 队列非空时消费者不得提前回收孤儿槽
  CHECK(!GlobalRegistry().TryReclaimOrphanedSlot(0U));

  // 消费者排空槽 0 后成功回收，状态转为 kFree 且 epoch 递增为 1
  RecordView v = buf0->queue.Front();
  CHECK(v.Valid());
  buf0->queue.Pop(v);
  CHECK(GlobalRegistry().TryReclaimOrphanedSlot(0U));

  const auto freed_word = GlobalRegistry().GetSlotEpochWord(0U);
  CHECK(freed_word.get<^^abi::SlotEpochSpec::state>() == abi::SlotState::kFree);
  CHECK(freed_word.get<^^abi::SlotEpochSpec::epoch>() == 1U);

  // 新线程注册立即复用槽 0 且继承 epoch == 1
  std::uint32_t reused_slot = 999U;
  ThreadBuffer* reused = GlobalRegistry().RegisterSlot(&reused_slot);
  CHECK(reused == buf0);
  CHECK(reused_slot == 0U);
  CHECK(reused->slot_epoch == 1U);

  for (std::uint32_t s : claimed_slots) {
    GlobalRegistry().RetireSlot(s);
    (void)GlobalRegistry().TryReclaimOrphanedSlot(s);
  }
  GlobalRegistry().ResetForTesting();
}

void TlsThreadExitRetirementTest() {
  // 验证真实工作线程退出时自动将槽置为 kOrphaned 并敲响门铃，且退出态禁止二次注册泄漏
  RetireCurrentThread();
  GlobalRegistry().ResetForTesting();
  ResetDoorbellForTesting();

  std::uint32_t worker_slot = 999U;
  std::thread worker([&]() {
    auto res = PrewarmCurrentThread();
    CHECK(res.has_value());
    worker_slot = (*res)->slot_index;
    CHECK(TrySetCurrentThreadName("alpha_worker_thread").has_value());
    CHECK((*res)->GetThreadName() == "alpha_worker_thread");

    // 超过 31 字节的线程名必须返回 ErrorCode::kInvalidArgument，严禁静默截断
    auto too_long = TrySetCurrentThreadName("0123456789abcdef0123456789abcdef");
    CHECK(!too_long.has_value() && too_long.error() == ErrorCode::kInvalidArgument);
    CHECK((*res)->GetThreadName() == "alpha_worker_thread");
  });
  worker.join();

  CHECK(worker_slot < kMaxThreads);
  const auto word = GlobalRegistry().GetSlotEpochWord(worker_slot);
  CHECK(word.get<^^abi::SlotEpochSpec::state>() == abi::SlotState::kOrphaned);
  CHECK(LoadDoorbell().get<^^abi::DoorbellSpec::ring_seq>() >= 1U);
  CHECK(GlobalRegistry().TryReclaimOrphanedSlot(worker_slot));

  // 模拟 TLS 析构阶段 t_thread_exiting = true 后再次尝试注册，必须返回 kShuttingDown / nullptr 且不占槽
  std::thread exiting_worker([]() {
    t_thread_exiting = true;
    CHECK(RegisterThreadSlow() == nullptr);
    auto prewarm = PrewarmCurrentThread();
    CHECK(!prewarm.has_value() && prewarm.error() == ErrorCode::kShuttingDown);
  });
  exiting_worker.join();
  CHECK(GlobalRegistry().ActiveSlotsBitmap().is_empty());

  GlobalRegistry().ResetForTesting();
}

void SeqlockConcurrentThreadNameTest() {
  // 验证并发更新与读取线程名时 Seqlock 原子载荷无撕裂且通过 TSan 检查
  RetireCurrentThread();
  GlobalRegistry().ResetForTesting();

  auto res = PrewarmCurrentThread();
  CHECK(res.has_value());
  ThreadBuffer* tb = *res;

  constexpr std::string_view name_a = "engine_io_worker_00000000000001";
  constexpr std::string_view name_b = "risk_gate_ matching_thread_0002";
  CHECK(tb->SetThreadName(name_a).has_value());

  std::atomic<bool> stop{false};
  std::thread writer([&]() {
    for (int i = 0; i < 5000; ++i) {
      CHECK(tb->SetThreadName((i & 1) == 0 ? name_a : name_b).has_value());
    }
    stop.store(true, std::memory_order_release);
  });

  while (!stop.load(std::memory_order_acquire)) {
    const ThreadName snap = tb->ReadThreadName();
    const std::string_view sv = snap.View();
    CHECK(sv == name_a || sv == name_b);
  }
  writer.join();

  RetireCurrentThread();
  GlobalRegistry().ResetForTesting();
}

void QueueFullDropAndBlockTest() {
  // 验证满队列时精确 ErrorCode 返回、kDrop 丢弃计数、回调重入保护与 kBlock 阻塞恢复
  RetireCurrentThread();
  GlobalRegistry().ResetForTesting();
  SetQueueFullPolicy(abi::QueueFullPolicy::kDrop);
  SetQueueFullCallback(&OnQueueFullTestCallback);
  g_callback_hits.store(0U, std::memory_order_relaxed);

  ThreadBuffer* tb = RegisterThreadSlow();
  CHECK(tb != nullptr);

  // 超大记录请求返回 kRecordTooLarge，且不计入丢弃记录数
  auto too_large = tb->TryReserveSlow(abi::kMaxRecordBlocks + 1U);
  CHECK(!too_large.has_value() && too_large.error() == ErrorCode::kRecordTooLarge);
  CHECK(tb->DroppedRecords() == 0U);

  // 生产者预留重入标志下返回 kConsumerBusy 并累加丢弃计数
  {
    ScopedReentrancyFlag prod_guard{kReentrancyInProducerReservation};
    auto reentrant = tb->TryReserveSlow(4U);
    CHECK(!reentrant.has_value() && reentrant.error() == ErrorCode::kConsumerBusy);
  }
  CHECK(tb->DroppedRecords() == 1U);

  // 填满整个 2 MiB 环形队列
  constexpr std::uint32_t chunk = abi::kMaxRecordBlocks;
  std::uint32_t chunks = 0U;
  for (;;) {
    std::byte* rec = tb->TryReserve(chunk);
    if (rec == nullptr && (rec = tb->queue.ReserveSlow(chunk)) == nullptr) {
      break;
    }
    SpscVarQueue::Publish(rec, abi::RecordHeaderWord{}
                                   .with<^^abi::RecordHeaderSpec::blocks>(chunk)
                                   .with<^^abi::RecordHeaderSpec::level>(abi::Level::kInfo)
                                   .with<^^abi::RecordHeaderSpec::payload_bytes>((chunk - 4U) * 8U));
    ++chunks;
  }
  CHECK(chunks >= 7U);

  // 此时再次调用 tb->TryReserveSlow 将触发一次回调（回调内重入调用被拦截）并返回 kQueueFull
  auto full_res = tb->TryReserveSlow(chunk, abi::Level::kInfo);
  CHECK(!full_res.has_value() && full_res.error() == ErrorCode::kQueueFull);
  CHECK(g_callback_hits.load(std::memory_order_relaxed) == 1U);
  CHECK(tb->DroppedRecords() >= 2U);
  CHECK(GlobalRegistry().GetSlotEpochWord(tb->slot_index).test<^^abi::SlotEpochSpec::overflowed>());

  // 切换为 kBlock 策略，但在消费者线程（kReentrancyInConsumer）下自动降级为 kDrop 防死锁
  SetQueueFullCallback(nullptr);
  SetQueueFullPolicy(abi::QueueFullPolicy::kBlock);
  {
    ScopedReentrancyFlag consumer_guard{kReentrancyInConsumer};
    auto downgraded = tb->TryReserveSlow(chunk, abi::Level::kInfo);
    CHECK(!downgraded.has_value() && downgraded.error() == ErrorCode::kQueueFull);
  }

  // 真正阻塞等待后台消费者线程 Pop 释放空间
  std::thread consumer([&]() {
    while (tb->BlockedEvents() == 0U) {
      std::this_thread::yield();
    }
    for (int i = 0; i < 2; ++i) {
      RecordView v = tb->queue.Front();
      CHECK(v.Valid());
      tb->queue.Pop(v);
    }
  });

  std::byte* unblocked = tb->ReserveSlow(chunk, abi::Level::kInfo);
  CHECK(unblocked != nullptr);
  CHECK(tb->BlockedEvents() == 1U);
  consumer.join();

  SetQueueFullPolicy(abi::QueueFullPolicy::kDrop);
  RetireCurrentThread();
  GlobalRegistry().ResetForTesting();
}

}  // namespace

int main() {
  SlotExhaustionAndReclaimTest();
  TlsThreadExitRetirementTest();
  SeqlockConcurrentThreadNameTest();
  QueueFullDropAndBlockTest();
  return 0;
}
