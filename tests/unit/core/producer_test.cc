#include <array>
#include <string>
#include <zerolog/core/consumer.hpp>
#include <zerolog/core/producer.hpp>

#include "support/check.hpp"
struct Failing {
  std::uint32_t value;
};
struct Recursive {
  std::uint32_t value;
};
static zerolog::ErrorCode recursive_log{};
static zerolog::ErrorCode recursive_poll{};
namespace zerolog::encode {
template <>
struct Codec<Failing> {
  static constexpr bool kFixed = true;
  static constexpr std::size_t kBytes = 4;
  static constexpr int kProblem = 0;
  static ErrorCode Encode(std::byte*&, std::size_t&, const Failing&) noexcept { return ErrorCode::kWriteFailed; }
  static ErrorCode Format(abi::DecodeCursor&, abi::FormatPresentation, std::uint8_t, abi::OutputAppender&) noexcept {
    return ErrorCode::kInvalidArgument;
  }
};
template <>
struct Codec<Recursive> : Codec<std::uint32_t> {
  static ErrorCode Encode(std::byte*& dest, std::size_t& cap, const Recursive& value) noexcept {
    recursive_log = TryInfo("nested");
    auto poll = Poll();
    recursive_poll = poll ? ErrorCode::kSuccess : poll.error();
    return Codec<std::uint32_t>::Encode(dest, cap, value.value);
  }
};
}  // namespace zerolog::encode
static int callbacks{};
static zerolog::ErrorCode callback_log{};
static zerolog::ErrorCode callback_poll{};
static void QueueFull(std::uint32_t, std::uint32_t, zerolog::abi::Level) noexcept {
  ++callbacks;
  static const std::array<char, 1024> bulk{};
  callback_log = zerolog::TryInfo("{}", std::string_view(bulk.data(), bulk.size()));
  auto poll = zerolog::Poll();
  callback_poll = poll ? zerolog::ErrorCode::kSuccess : poll.error();
  zerolog::core::SetLevel(zerolog::abi::Level::kTrace);
}
int main() {
  using namespace zerolog;
  using namespace zerolog::core;
  ResetDoorbellForTesting();
  SetLevel(abi::Level::kOff);
  // Fire-and-forget calls return void and never require caller status handling.
  Trace("ignored trace");
  Debug("ignored debug");
  Info("ignored filtered status");
  Warn("ignored warn");
  Error("ignored error");
  Fatal("ignored fatal");
  Log<abi::Level::kInfo>("ignored generic log");
  int evaluated = 0;
  CHECK(TryInfo("{}", Lazy{[&]() noexcept {
                  ++evaluated;
                  return 7U;
                }}) == ErrorCode::kSuccess);
  CHECK(evaluated == 0 && t_buffer == nullptr);
  CHECK(IfEnabled(abi::Level::kInfo, [&]() noexcept { ++evaluated; }) == ErrorCode::kSuccess && evaluated == 0);
  SetLevel(abi::Level::kTrace);
  auto warm = PrewarmCurrentThread();
  CHECK(warm);
  auto* buf = *warm;
  {
    SpanGuard span{123};
    Info("hello {} {:x}", std::uint64_t{11}, std::uint64_t{42});
  }
  auto front = buf->queue.Front();
  CHECK(front.Valid() && front.Cursor().remaining == 16);
  CHECK(front.Prefix()->span_id == 123);
  char body[64]{};
  std::size_t len = 0;
  abi::OutputAppender out{body, &len, sizeof(body)};
  CHECK(front.Prefix()->info->decode(front.Prefix()->info, front.Cursor(), out) == ErrorCode::kSuccess);
  CHECK(std::string_view(body, len) == "hello 11 2a");
  buf->queue.Pop(front);
  const auto before = buf->queue.ProducerMeta().raw();
  CHECK(TryInfo("{}", Failing{9}) == ErrorCode::kWriteFailed);
  CHECK(!buf->queue.Front().Valid() && buf->queue.ProducerMeta().raw() == before);
  CHECK(TryInfo("{}", Recursive{17}) == ErrorCode::kSuccess);
  CHECK(recursive_log == ErrorCode::kConsumerBusy && recursive_poll == ErrorCode::kConsumerBusy);
  buf->queue.Pop(buf->queue.Front());
  CHECK(TryInfo("{}", Lazy{[&]() noexcept {
                  ++evaluated;
                  return std::string("snapshot");
                }}) == ErrorCode::kSuccess);
  CHECK(evaluated == 1);
  buf->queue.Pop(buf->queue.Front());
  std::string oversized(abi::kMaxRecordBlocks * abi::Block::value, 'x');
  CHECK(TryInfo("{}", std::string_view(oversized)) == ErrorCode::kRecordTooLarge);
  CHECK(!buf->queue.Front().Valid());
  std::string bulk(1024, 'b');
  ErrorCode status = ErrorCode::kSuccess;
  for (int n = 0; n < 10000 && status == ErrorCode::kSuccess; ++n) status = TryInfo("{}", std::string_view(bulk));
  CHECK(status == ErrorCode::kQueueFull);
  SetQueueFullCallback(&QueueFull);
  CHECK(TryInfo("{}", std::string_view(bulk)) == ErrorCode::kQueueFull);
  CHECK(callbacks == 1 && callback_log == ErrorCode::kQueueFull && callback_poll == ErrorCode::kConsumerBusy);
  SetQueueFullPolicy(abi::QueueFullPolicy::kBlock);
  {
    ScopedReentrancyFlag guard{kInConsumer};
    CHECK(TryInfo("{}", std::string_view(bulk)) == ErrorCode::kQueueFull);
  }
  SetQueueFullCallback(nullptr);
  SetQueueFullPolicy(abi::QueueFullPolicy::kDrop);
  t_thread_exiting = true;
  CHECK(TryInfo("exit") == ErrorCode::kShuttingDown);
  t_thread_exiting = false;
  for (std::uint32_t i = 1; i < 64; ++i) CHECK(GlobalRegistry().TryRegisterSlot());
  t_buffer = nullptr;
  CHECK(TryInfo("overflow") == ErrorCode::kTooManyThreads);
  t_buffer = buf;
}
