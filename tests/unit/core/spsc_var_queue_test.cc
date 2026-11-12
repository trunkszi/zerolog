#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <zerolog/core/spsc_var_queue.hpp>

#include "support/check.hpp"

namespace {

using namespace zerolog;
using namespace zerolog::core;

void WriteTestRecord(std::byte* rec, std::uint32_t blocks, std::uint64_t seq) noexcept {
  const std::uint32_t payload_bytes = (blocks - 4U) * 8U;
  std::uint64_t tsc = 1000U + seq;
  std::uint64_t span = 2000U + seq;
  std::memcpy(rec + 16, &tsc, sizeof(tsc));
  std::memcpy(rec + 24, &span, sizeof(span));
  for (std::uint32_t b = 4U; b < blocks; ++b) {
    const std::uint64_t word = (seq << 16) ^ b;
    std::memcpy(rec + static_cast<std::size_t>(b) * 8U, &word, sizeof(word));
  }
  const auto header = abi::RecordHeaderWord{}
                          .with<^^abi::RecordHeaderSpec::blocks>(blocks)
                          .with<^^abi::RecordHeaderSpec::level>(abi::Level::kInfo)
                          .with<^^abi::RecordHeaderSpec::payload_bytes>(payload_bytes);
  SpscVarQueue::Publish(rec, header);
}

void FastReservePublishPopTest() {
  // 验证快路径预留、发布与消费游标推进的基本闭环
  auto q_res = SpscVarQueue::Create();
  CHECK(q_res.has_value());
  auto& q = *q_res;

  CHECK(!q.Front().Valid());
  std::byte* rec = q.TryReserve(6U);
  CHECK(rec != nullptr);
  WriteTestRecord(rec, 6U, 42U);

  RecordView view = q.Front();
  CHECK(view.Valid());
  CHECK(view.header.get<^^abi::RecordHeaderSpec::blocks>() == 6U);
  CHECK(view.Prefix()->tsc == 1042U);
  CHECK(view.Prefix()->span_id == 2042U);
  CHECK(view.Cursor().remaining == 16U);

  q.Pop(view);
  CHECK(!q.Front().Valid());
  CHECK(q.ReadIdx() == 6U);

  // 验证不足 32 字节（如 8 字节）的参数负载不会被 RecordView::Cursor() 截断归零
  std::byte* small_rec = q.TryReserve(5U);
  CHECK(small_rec != nullptr);
  WriteTestRecord(small_rec, 5U, 43U);
  RecordView small_view = q.Front();
  CHECK(small_view.Valid());
  CHECK(small_view.Cursor().remaining == 8U);
  q.Pop(small_view);
}

void CancelReservationAndReuseTest() {
  // 验证预留取消回滚游标并清零脏数据，防止阻塞消费者或在复用周期产生幽灵记录
  auto q_res = SpscVarQueue::Create();
  CHECK(q_res.has_value());
  auto& q = *q_res;

  const auto zero_err = q.TryReserveSlow(0U);
  CHECK(!zero_err.has_value() && zero_err.error() == ErrorCode::kRecordTooLarge);
  const auto huge_err = q.TryReserveSlow(abi::kMaxRecordBlocks + 1U);
  CHECK(!huge_err.has_value() && huge_err.error() == ErrorCode::kRecordTooLarge);

  std::byte* dirty = q.TryReserve(6U);
  CHECK(dirty != nullptr);
  std::memset(dirty, 0xFF, 6U * 8U);
  CHECK(q.ProducerMeta().get<^^abi::ProducerMetaSpec::write_idx>() == 6U);
  CHECK(q.ProducerMeta().get<^^abi::ProducerMetaSpec::free_blocks>() == abi::kMaxRecordBlocks - 6U);

  q.CancelReservation(dirty, 6U);
  CHECK(q.ProducerMeta().get<^^abi::ProducerMetaSpec::write_idx>() == 0U);
  CHECK(q.ProducerMeta().get<^^abi::ProducerMetaSpec::free_blocks>() == abi::kMaxRecordBlocks);
  CHECK(!q.Front().Valid());

  // 取消后同一位置重新预留并发布有效记录，消费者正常读取不卡死
  std::byte* clean = q.TryReserve(6U);
  CHECK(clean == dirty);
  WriteTestRecord(clean, 6U, 77U);
  RecordView v = q.Front();
  CHECK(v.Valid());
  CHECK(v.Prefix()->tsc == 1077U);
  q.Pop(v);
  CHECK(!q.Front().Valid());

  // 在非零偏移写入脏字节后取消预留并执行 ResetForReuse()，确保无幽灵记录
  std::byte* second_dirty = q.TryReserve(8U);
  CHECK(second_dirty != nullptr);
  std::memset(second_dirty + 8, 0xAB, 7U * 8U);
  q.CancelReservation(second_dirty, 8U);
  q.ResetForReuse();
  CHECK(!q.Front().Valid());

  // 并发场景：生产者交替取消脏预留与发布有效记录，消费者持续轮询 Front()/Pop()，验证 TSan 无数据竞争
  std::atomic<bool> start_cancel{false};
  constexpr std::uint64_t total_published = 2000U;
  std::thread producer([&]() {
    while (!start_cancel.load(std::memory_order_acquire)) {
    }
    for (std::uint64_t seq = 1U; seq <= total_published; ++seq) {
      std::byte* aborted = q.TryReserve(6U);
      while (aborted == nullptr) {
        aborted = q.ReserveSlow(6U);
      }
      std::memset(aborted + 8, 0xCC, 5U * 8U);
      q.CancelReservation(aborted, 6U);

      std::byte* committed = q.TryReserve(6U);
      while (committed == nullptr) {
        committed = q.ReserveSlow(6U);
      }
      WriteTestRecord(committed, 6U, seq);
    }
  });

  start_cancel.store(true, std::memory_order_release);
  for (std::uint64_t expected = 1U; expected <= total_published; ++expected) {
    RecordView cv{};
    while (!(cv = q.Front()).Valid()) {
    }
    CHECK(cv.Prefix()->tsc == 1000U + expected);
    q.Pop(cv);
  }
  producer.join();
  CHECK(!q.Front().Valid());
}

void TailWrapSentinelTest() {
  // 验证环形尾部空间不足时写入回绕哨兵并从头部分配
  auto q_res = SpscVarQueue::Create();
  CHECK(q_res.has_value());
  auto& q = *q_res;

  constexpr auto ring_cap = static_cast<std::uint32_t>(abi::RingBlocks::value);
  constexpr std::uint32_t chunk = abi::kMaxRecordBlocks;

  for (std::uint32_t i = 0; i < 7U; ++i) {
    std::byte* rec = q.TryReserve(chunk);
    if (rec == nullptr) {
      rec = q.ReserveSlow(chunk);
    }
    CHECK(rec != nullptr);
    WriteTestRecord(rec, chunk, i + 1U);
    RecordView v = q.Front();
    CHECK(v.Valid());
    q.Pop(v);
  }

  const std::uint32_t near_end = ring_cap - (7U * chunk) - 10U;
  std::byte* tail_rec = q.ReserveSlow(near_end);
  CHECK(tail_rec != nullptr);
  WriteTestRecord(tail_rec, near_end, 99U);
  RecordView tail_view = q.Front();
  CHECK(tail_view.Valid());
  q.Pop(tail_view);

  // 此时尾部仅剩 9 个可用块，请求 16 块触发回绕哨兵写入并跳回块 0
  std::byte* wrapped = q.TryReserve(16U);
  CHECK(wrapped == nullptr);
  wrapped = q.ReserveSlow(16U);
  CHECK(wrapped == q.Base());
  WriteTestRecord(wrapped, 16U, 100U);

  RecordView wrapped_view = q.Front();
  CHECK(wrapped_view.Valid());
  CHECK(wrapped_view.data == q.Base());
  CHECK(wrapped_view.Prefix()->tsc == 1100U);
  q.Pop(wrapped_view);
  CHECK(q.ReadIdx() == 16U);
}

void WatermarkHysteresisCycleTest() {
  // 验证 50% 水位触发单次门铃与低于 25% 水位后重新武装的迟滞闭环
  auto q_res = SpscVarQueue::Create();
  CHECK(q_res.has_value());
  auto& q = *q_res;

  constexpr std::uint32_t step = 16384U;
  std::uint32_t notify_count = 0U;

  // 连续写入 8 次 16384 块（共 131072 块 == 50% 容量），仅在第 8 次跨越 50% 时触发一次通知
  for (std::uint32_t i = 0; i < 10U; ++i) {
    std::byte* rec = q.TryReserve(step);
    if (rec == nullptr) {
      rec = q.ReserveSlow(step);
    }
    CHECK(rec != nullptr);
    WriteTestRecord(rec, step, i);
    if (q.ConsumeNotifyAfterPublish()) {
      ++notify_count;
      CHECK(i == 7U);
    }
  }
  CHECK(notify_count == 1U);
  CHECK(q.ProducerMeta().test<^^abi::ProducerMetaSpec::watermark_rung>());

  // 消费至占用低于 25%（剩余 3 * 16384 = 49152 < 65536）
  for (std::uint32_t i = 0; i < 7U; ++i) {
    RecordView v = q.Front();
    CHECK(v.Valid());
    q.Pop(v);
  }

  // 下一次慢路径预留将重武装 watermark_rung，再次跨越 50% 时触发第二次通知
  std::uint32_t second_cycle_notifies = 0U;
  for (std::uint32_t i = 0; i < 6U; ++i) {
    std::byte* rec = q.ReserveSlow(step);
    CHECK(rec != nullptr);
    WriteTestRecord(rec, step, 100U + i);
    if (q.ConsumeNotifyAfterPublish()) {
      ++second_cycle_notifies;
    }
  }
  CHECK(second_cycle_notifies == 1U);
}

void ConcurrentSpscStressTest() {
  // 验证单生产者单消费者并发跨多次环形回绕的数据完整性与顺序性
  auto q_res = SpscVarQueue::Create();
  CHECK(q_res.has_value());
  auto& q = *q_res;

  constexpr std::uint64_t total_records = 60000U;
  std::atomic<bool> start{false};

  std::thread producer([&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (std::uint64_t seq = 1U; seq <= total_records; ++seq) {
      const std::uint32_t blocks = 4U + static_cast<std::uint32_t>(seq & 7U);
      std::byte* rec = q.TryReserve(blocks);
      while (rec == nullptr) {
        rec = q.ReserveSlow(blocks);
      }
      WriteTestRecord(rec, blocks, seq);
      (void)q.ConsumeNotifyAfterPublish();
    }
  });

  start.store(true, std::memory_order_release);
  for (std::uint64_t expected = 1U; expected <= total_records; ++expected) {
    RecordView view{};
    while (!(view = q.Front()).Valid()) {
    }
    const std::uint32_t expected_blocks = 4U + static_cast<std::uint32_t>(expected & 7U);
    CHECK(view.header.get<^^abi::RecordHeaderSpec::blocks>() == expected_blocks);
    CHECK(view.Prefix()->tsc == 1000U + expected);
    CHECK(view.Prefix()->span_id == 2000U + expected);
    for (std::uint32_t b = 4U; b < expected_blocks; ++b) {
      std::uint64_t word = 0U;
      std::memcpy(&word, view.data + static_cast<std::size_t>(b) * 8U, sizeof(word));
      CHECK(word == ((expected << 16) ^ b));
    }
    q.Pop(view);
  }

  producer.join();
  CHECK(!q.Front().Valid());
}

}  // namespace

int main() {
  FastReservePublishPopTest();
  CancelReservationAndReuseTest();
  TailWrapSentinelTest();
  WatermarkHysteresisCycleTest();
  ConcurrentSpscStressTest();
  return 0;
}
