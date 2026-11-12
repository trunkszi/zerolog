#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <uarch/core/flow/delegate.hpp>
static void VerifyLateConsumer();
struct LateConsumerCheck {
  ~LateConsumerCheck() { VerifyLateConsumer(); }
};
static LateConsumerCheck late_consumer_check;
#include <zerolog/zerolog.hpp>

#include "support/check.hpp"
using namespace zerolog;

static_assert(
    std::same_as<zerolog::core::WriteCallback, uarch::Callback<zerolog::Result<std::uint32_t>(std::span<const char>)>>);
static_assert(std::same_as<zerolog::WriteCallback, zerolog::core::WriteCallback>);
static_assert(sizeof(zerolog::core::WriteCallback) == 16);
static_assert(alignof(zerolog::core::WriteCallback) == 8);
static_assert(uarch::NoPadding<zerolog::core::WriteCallback> && uarch::GprPair<zerolog::core::WriteCallback>);
static_assert(std::same_as<uarch::CallArg<std::span<const char>>, std::span<const char>>);
static_assert(std::atomic<zerolog::core::QueueFullCallback>::is_always_lock_free &&
              sizeof(zerolog::core::QueueFullCallback) == 8);
static void VerifyLateConsumer() {
  auto owner = zerolog::AcquireConsumer();
  CHECK(owner);
  CHECK(owner->Core());
  CHECK(owner->Core().value()->Output().TotalUnflushedBytes() == 0);
}
struct ConcurrentFailure {
  std::uint32_t value;
};
namespace zerolog::encode {
template <>
struct Codec<ConcurrentFailure> {
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = 4;
  static constexpr int kProblem = 0;
  static ErrorCode Encode(std::byte*&, std::size_t&, const ConcurrentFailure&) noexcept { return ErrorCode::kWriteFailed; }
  static ErrorCode Format(abi::DecodeCursor&, abi::FormatPresentation, std::uint8_t, abi::OutputAppender&) noexcept {
    return ErrorCode::kInvalidArgument;
  }
};
}  // namespace zerolog::encode

static ErrorCode ValueDecode(const abi::StaticRecordInfo*, abi::DecodeCursor cursor, abi::OutputAppender& out) noexcept {
  if (cursor.remaining != 4) return ErrorCode::kInvalidArgument;
  std::uint32_t value{};
  std::memcpy(&value, cursor.data, 4);
  if (auto error = out.Push('v'); error != ErrorCode::kSuccess) return error;
  return format::FormatUint(out, value);
}
static ErrorCode ExpandDecode(const abi::StaticRecordInfo*, abi::DecodeCursor cursor, abi::OutputAppender& out) noexcept {
  std::uint32_t count{};
  if (cursor.remaining != 4) return ErrorCode::kInvalidArgument;
  std::memcpy(&count, cursor.data, 4);
  std::array<char, 4096> block{};
  while (count != 0) {
    const auto n = std::min(count, 4096U);
    if (auto error = out.Append(block.data(), n); error != ErrorCode::kSuccess) return error;
    count -= n;
  }
  return ErrorCode::kSuccess;
}
static ErrorCode BadDecode(const abi::StaticRecordInfo*, abi::DecodeCursor, abi::OutputAppender& out) noexcept {
  const auto error = out.Append("discard", 7);
  return error == ErrorCode::kSuccess ? ErrorCode::kInvalidArgument : error;
}
static void Push(core::ThreadBuffer& buf, abi::StaticRecordInfo& info, std::uint64_t tsc, std::uint32_t value) {
  auto* record = buf.TryReserve(5);
  if (record == nullptr) {
    auto reserved = buf.TryReserveSlow(5, abi::Level::kInfo);
    CHECK(reserved);
    record = *reserved;
  }
  const abi::RecordPrefix prefix{0, &info, tsc, 0};
  std::memcpy(record + 8, reinterpret_cast<const std::byte*>(&prefix) + 8, 24);
  std::memcpy(record + 32, &value, 4);
  const auto header = abi::RecordHeaderWord{}
                          .with<^^abi::RecordHeaderSpec::blocks>(5)
                          .with<^^abi::RecordHeaderSpec::payload_bytes>(4)
                          .with<^^abi::RecordHeaderSpec::level>(abi::Level::kInfo);
  buf.PublishAndNotify(record, header, abi::Level::kInfo);
}
static std::uint32_t writes{};
static Result<std::uint32_t> Count(std::span<const char> bytes) noexcept {
  ++writes;
  return static_cast<std::uint32_t>(bytes.size());
}
static void Clear(format::OutputBuffer& output) {
  if (auto slab = output.Freeze()) CHECK(output.CompleteFrozen(*slab, slab->length));
  CHECK(output.TotalUnflushedBytes() == 0);
}
struct Capture {
  std::string text;
  bool nested{};

  Result<std::uint32_t> Write(std::span<const char> bytes) noexcept {
    if (!nested) {
      nested = true;
      auto nested_res = Poll();
      CHECK(!nested_res && nested_res.error() == ErrorCode::kConsumerBusy);
    }
    const auto n = std::min<std::size_t>(bytes.size(), 3);
    text.append(bytes.data(), n);
    return static_cast<std::uint32_t>(n);
  }
};
static Result<std::uint32_t> Reject(std::span<const char>) noexcept { return std::unexpected(ErrorCode::kWouldBlock); }
static Result<std::uint32_t> ZeroWrite(std::span<const char>) noexcept { return 0; }
static Result<std::uint32_t> OverAck(std::span<const char> bytes) noexcept {
  return static_cast<std::uint32_t>(bytes.size()) + 1;
}
static Result<std::uint32_t> IoError(std::span<const char>) noexcept { return std::unexpected(ErrorCode::kWriteFailed); }
int main() {
  using namespace zerolog::core;
  SetLevel(abi::Level::kTrace);
  auto consumer = ConsumerCore::Create();
  CHECK(consumer);
  CHECK(consumer->SetWriter(WriteCallback{}) == ErrorCode::kInvalidArgument);
  CHECK(consumer->SetWriter(uarch::bind<&Count>()) == ErrorCode::kSuccess);
  abi::StaticRecordInfo info{};
  info.decode = &ValueDecode;
  const auto base = sys::ReadTsc();
  for (std::uint32_t i = 0; i < 64; ++i) {
    std::uint32_t slot{};
    auto buf = GlobalRegistry().TryRegisterSlot(&slot);
    CHECK(buf && slot == i);
    Push(**buf, info, base + 64 - i, i);
    GlobalRegistry().RetireSlot(slot);
  }
  auto batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 64 && writes == 0);
  CHECK(GlobalRegistry().ActiveSlotsBitmap().is_empty());
  const auto bytes = consumer->Output().Pending();
  const std::string_view text(bytes.data(), bytes.size());
  CHECK(text.find("v63\n") < text.find("v62\n") && text.find("v1\n") < text.find("v0\n"));
  Clear(consumer->Output());
  abi::StaticRecordInfo expand{};
  expand.decode = &ExpandDecode;
  std::uint32_t count = format::kMaxPendingBytes - 256;
  abi::RecordPrefix prefix{abi::RecordHeaderWord{}.with<^^abi::RecordHeaderSpec::level>(abi::Level::kInfo).raw(), &expand, base,
                           0};
  CHECK(consumer->Output().FormatRecord(prefix, {reinterpret_cast<const std::byte*>(&count), 4, 0}, 0, 0, 0) ==
        ErrorCode::kSuccess);
  auto frozen = consumer->Output().Freeze();
  CHECK(frozen);
  std::uint32_t slot{};
  auto buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  Push(**buf, expand, base, 1024 * 1024);
  Push(**buf, info, base + 1, 7);
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc(), {4096, format::kMaxPendingBytes});
  CHECK(batch && batch->records == 0 &&
        abi::DrainFlagsWord::from_raw(batch->flags).test<^^abi::DrainFlagsSpec::backpressure>());
  CHECK(consumer->RetainedHeads() == 1 && !GlobalRegistry().TryReclaimOrphanedSlot(slot) && writes == 0);
  CHECK(consumer->Output().TotalUnflushedBytes() <= format::kMaxPendingBytes);
  CHECK(consumer->Output().CompleteFrozen(*frozen, frozen->length));
  batch = consumer->DrainBatch(sys::ReadTsc(), {4096, format::kMaxPendingBytes});
  CHECK(batch && batch->records == 2);
  CHECK(consumer->RetainedHeads() == 0 && GlobalRegistry().ActiveSlotsBitmap().is_empty());
  Clear(consumer->Output());
  buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  Push(**buf, expand, base, format::kMaxPendingBytes + 1);
  Push(**buf, info, base + 1, 8);
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(!batch && batch.error() == ErrorCode::kRecordTooLarge);
  CHECK((*buf)->queue.Front().Valid() && consumer->Output().TotalUnflushedBytes() == 0);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 1);
  Clear(consumer->Output());
  abi::StaticRecordInfo bad{};
  bad.decode = &BadDecode;
  buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  Push(**buf, bad, base, 0);
  Push(**buf, info, base + 1, 9);
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(!batch && batch.error() == ErrorCode::kInvalidArgument);
  CHECK(consumer->Output().PendingBytes() == 0 && (*buf)->queue.Front().Prefix()->info == &info);
  batch = consumer->DrainBatch(sys::ReadTsc(), {0, 1024});
  CHECK(batch && batch->records == 0);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 1);
  Clear(consumer->Output());
  // A single legal record must make progress even when it exceeds the soft byte budget.
  buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  Push(**buf, expand, base, 200000);
  Push(**buf, info, base + 1, 10);
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc(), {4096, 64 * 1024});
  CHECK(batch && batch->records == 1 && batch->bytes > 64 * 1024);
  CHECK(abi::DrainFlagsWord::from_raw(batch->flags).test<^^abi::DrainFlagsSpec::budget_exhausted>());
  CHECK(abi::DrainFlagsWord::from_raw(batch->flags).test<^^abi::DrainFlagsSpec::more_pending>());
  Clear(consumer->Output());
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 1);
  Clear(consumer->Output());
  // A short corrupted header occupies the minimum four-block record extent.
  buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  auto* corrupted = (*buf)->TryReserve(4);
  CHECK(corrupted);
  SpscVarQueue::Publish(corrupted, abi::RecordHeaderWord{}.with<^^abi::RecordHeaderSpec::blocks>(3));
  Push(**buf, info, base + 1, 11);
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(!batch && batch.error() == ErrorCode::kInvalidArgument);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 1);
  Clear(consumer->Output());
  buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  abi::StaticRecordInfo* null_info = nullptr;
  Push(**buf, info, base, 0);
  std::memcpy((*buf)->queue.Base() + 8, &null_info, sizeof(abi::StaticRecordInfo*));
  Push(**buf, info, base + 1, 12);
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(!batch && batch.error() == ErrorCode::kInvalidArgument);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 1);
  Clear(consumer->Output());
  // An excessive advertised extent is bounded before scrubbing the mapped ring.
  buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  corrupted = (*buf)->TryReserve(abi::kMaxRecordBlocks);
  if (corrupted == nullptr) {
    auto reserved = (*buf)->TryReserveSlow(abi::kMaxRecordBlocks, abi::Level::kInfo);
    CHECK(reserved);
    corrupted = *reserved;
  }
  SpscVarQueue::Publish(corrupted, abi::RecordHeaderWord{}.with<^^abi::RecordHeaderSpec::blocks>(abi::kMaxRecordBlocks + 1));
  Push(**buf, info, base + 1, 13);
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(!batch && batch.error() == ErrorCode::kInvalidArgument);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 1);
  Clear(consumer->Output());
  buf = GlobalRegistry().TryRegisterSlot(&slot);
  CHECK(buf);
  (*buf)->queue.Pop(SpscVarQueue::kRingCap - 4);
  Push(**buf, info, base + 1, 14);
  corrupted = (*buf)->queue.Base() + (SpscVarQueue::kRingCap - 4) * abi::Block::value;
  SpscVarQueue::Publish(corrupted, abi::RecordHeaderWord{}.with<^^abi::RecordHeaderSpec::blocks>(abi::kMaxRecordBlocks + 1));
  GlobalRegistry().RetireSlot(slot);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(!batch && batch.error() == ErrorCode::kInvalidArgument);
  batch = consumer->DrainBatch(sys::ReadTsc());
  CHECK(batch && batch->records == 1);
  Clear(consumer->Output());
  CHECK(writes == 0);
  {
    auto owner = AcquireConsumer();
    CHECK(owner);
    CHECK(owner->SetWriter(WriteCallback{}) == ErrorCode::kInvalidArgument);
    CHECK(owner->SetWriter(uarch::bind<&Count>()) == ErrorCode::kSuccess);
    static const bool flags[200000]{};
    CHECK(TryInfo("flags={}", flags) == ErrorCode::kSuccess);
    auto large = owner->PollBatch({4096, 64 * 1024});
    CHECK(large && large->records == 1 && large->bytes > 64 * 1024);
    CHECK(abi::DrainFlagsWord::from_raw(large->flags).test<^^abi::DrainFlagsSpec::budget_exhausted>());
    auto next = owner->PollBatch({4096, 64 * 1024});
    CHECK(next && next->records == 0);
  }
  {
    auto owner = AcquireConsumer();
    CHECK(owner);
    CHECK(owner->SetWriter(uarch::bind<&Reject>()) == ErrorCode::kSuccess);
    Info("survives handoff");
    auto failed = owner->Poll();
    CHECK(!failed && failed.error() == ErrorCode::kWouldBlock);
    CHECK(owner->SetWriter(uarch::bind<&ZeroWrite>()) == ErrorCode::kSuccess);
    failed = owner->Poll();
    CHECK(!failed && failed.error() == ErrorCode::kWriteFailed);
    CHECK(owner->SetWriter(uarch::bind<&OverAck>()) == ErrorCode::kSuccess);
    failed = owner->Poll();
    CHECK(!failed && failed.error() == ErrorCode::kInvalidArgument);
    CHECK(owner->SetWriter(uarch::bind<&IoError>()) == ErrorCode::kSuccess);
    failed = owner->Poll();
    CHECK(!failed && failed.error() == ErrorCode::kWriteFailed);
    auto external_poll = Poll();
    CHECK(!external_poll && external_poll.error() == ErrorCode::kLeaseHeld);
  }
  Capture capture;
  {
    auto owner = AcquireConsumer();
    CHECK(owner);
    CHECK(owner->SetWriter(uarch::bind<&Capture::Write>(capture)) == ErrorCode::kSuccess);
    auto poll = owner->Poll(true);
    CHECK(poll && capture.text.find("survives handoff") != std::string::npos);
    CHECK(std::count(capture.text.begin(), capture.text.end(), '\n') == 1);
    auto moved = std::move(*owner);
    CHECK(!owner->DrainBatch() && owner->DrainBatch().error() == ErrorCode::kConsumerBusy);
    CHECK(moved.SetOutputFd(2) == ErrorCode::kSuccess);
  }
  CHECK(Poll());
  {
    auto owner = AcquireConsumer();
    CHECK(owner);
    int fds[2]{};
    CHECK(::pipe(fds) == 0);
    CHECK(owner->SetWriter(uarch::bind<&Count>()) == ErrorCode::kSuccess);
    const std::uint32_t writes_before = writes;
    CHECK(owner->SetOutputFd(fds[1]) == ErrorCode::kSuccess);
    Info("pipe target");
    CHECK(owner->Poll(true));
    CHECK(writes == writes_before);
    std::array<char, 2048> captured{};
    const auto size = ::read(fds[0], captured.data(), captured.size());
    CHECK(size > 0);
    CHECK(std::string_view(captured.data(), static_cast<std::size_t>(size)).find("pipe target") != std::string_view::npos);
    CHECK(owner->SetOutputFd(2) == ErrorCode::kSuccess);
    CHECK(::close(fds[0]) == 0 && ::close(fds[1]) == 0);
  }
  {
    auto owner = AcquireConsumer();
    CHECK(owner);
    CHECK(owner->SetWriter(uarch::bind<&Count>()) == ErrorCode::kSuccess);
    uarch::Isolated<std::atomic<std::uint32_t>> finished{0};
    uarch::Isolated<std::atomic<ErrorCode>> error{ErrorCode::kSuccess};
    std::array<std::thread, 4> producers;
    for (std::uint32_t i = 0; i < 4; ++i)
      producers[i] = std::thread([&, i] {
        for (std::uint32_t n = 0; n < 1000; ++n) {
          const auto failed = TryInfo("failed {}", ConcurrentFailure{n});
          if (failed != ErrorCode::kWriteFailed) {
            error->store(failed, std::memory_order_release);
            break;
          }
          const auto status = TryInfo("concurrent {} {}", i, n);
          if (status != ErrorCode::kSuccess) {
            error->store(status, std::memory_order_release);
            break;
          }
        }
        finished->fetch_add(1, std::memory_order_release);
      });
    std::uint32_t records = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (finished->load(std::memory_order_acquire) != 4 || records != 4000) {
      auto poll = owner->PollBatch({256, 1024 * 1024}, true);
      CHECK(poll);
      records += poll->records;
      if (finished->load(std::memory_order_acquire) == 4 && error->load(std::memory_order_acquire) != ErrorCode::kSuccess)
        break;
      CHECK(std::chrono::steady_clock::now() < deadline);
      if (poll->records == 0) std::this_thread::yield();
    }
    for (auto& producer : producers) producer.join();
    CHECK(records == 4000 && error->load() == ErrorCode::kSuccess);
    CHECK(owner->SetOutputFd(2) == ErrorCode::kSuccess);
  }
}
