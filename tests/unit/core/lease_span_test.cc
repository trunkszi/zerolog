#include <thread>
#include <zerolog/core/lease.hpp>
#include <zerolog/core/span.hpp>

#include "support/check.hpp"

namespace {

using namespace zerolog;
using namespace zerolog::core;

void ExternalLeaseAndPollerMutexTest() {
  // 验证外部消费者租约与内部 Poller 启动状态机的互斥及世代推进
  ResetLeaseStateForTesting();

  {
    auto ext = TryAcquireExternalLease();
    CHECK(ext.has_value());
    CHECK(ext->Valid());
    CHECK(ext->IsExternal());
    CHECK(ext->Generation() == 1U);

    // 持有外部租约时再次获取外部租约或启动 Poller 均返回 kLeaseHeld
    auto dup = TryAcquireExternalLease();
    CHECK(!dup.has_value() && dup.error() == ErrorCode::kLeaseHeld);
    auto poller = TryStartPoller();
    CHECK(!poller.has_value() && poller.error() == ErrorCode::kLeaseHeld);
  }

  // 外部租约析构释放后，Poller 预留启动成功并阻止并发外部租约抢占
  {
    auto poller_lease = TryStartPoller();
    CHECK(poller_lease.has_value());
    CHECK(poller_lease->Valid());
    CHECK(!poller_lease->IsExternal());
    CHECK(poller_lease->Generation() == 2U);

    auto ext_race = TryAcquireExternalLease();
    CHECK(!ext_race.has_value() && ext_race.error() == ErrorCode::kConsumerBusy);

    CHECK(CommitPollerStart(*poller_lease).has_value());
    CHECK(LoadConsumerState().test<^^abi::ConsumerStateSpec::poller_running>());
  }

  CHECK(!LoadConsumerState().test<^^abi::ConsumerStateSpec::active>());
  CHECK(!LoadConsumerState().test<^^abi::ConsumerStateSpec::poller_running>());
}

void LeasePoisoningPermanentTest() {
  // 验证租约毒化后拒绝一切后续外部租约或 Poller 启动请求
  ResetLeaseStateForTesting();

  auto ext = TryAcquireExternalLease();
  CHECK(ext.has_value());
  const auto poisoned_state = ext->Poison();
  CHECK(poisoned_state.test<^^abi::ConsumerStateSpec::poisoned>());
  CHECK(!poisoned_state.test<^^abi::ConsumerStateSpec::active>());

  auto next_ext = TryAcquireExternalLease();
  CHECK(!next_ext.has_value() && next_ext.error() == ErrorCode::kLeasePoisoned);

  auto next_poller = TryStartPoller();
  CHECK(!next_poller.has_value() && next_poller.error() == ErrorCode::kLeasePoisoned);

  ResetLeaseStateForTesting();
}

void SpanGuardNestedLexicalTest() {
  // 验证 SpanGuard 嵌套词法作用域的 LIFO 恢复与线程隔离
  CHECK(CurrentSpanId() == 0U);
  {
    SpanGuard outer(101U);
    CHECK(CurrentSpanId() == 101U);
    CHECK(outer.SpanId() == 101U);
    CHECK(outer.PreviousSpanId() == 0U);
    {
      SpanGuard inner(202U);
      CHECK(CurrentSpanId() == 202U);
      CHECK(inner.PreviousSpanId() == 101U);

      std::thread other([]() {
        CHECK(CurrentSpanId() == 0U);
        SpanGuard thread_span(999U);
        CHECK(CurrentSpanId() == 999U);
      });
      other.join();
      CHECK(CurrentSpanId() == 202U);
    }
    CHECK(CurrentSpanId() == 101U);
  }
  CHECK(CurrentSpanId() == 0U);
}

}  // namespace

int main() {
  ExternalLeaseAndPollerMutexTest();
  LeasePoisoningPermanentTest();
  SpanGuardNestedLexicalTest();
  return 0;
}
