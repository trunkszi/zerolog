# zerolog

[English](README.md)

面向超低延迟场景的 C++26 结构化日志库：基于每线程无锁 2 MiB 大页 SPSC 环形队列、C++26 P2996/P3491 编译期静态反射与异步 `io_uring` / `futex2` 后台消费格式化，将生产者热路径开销降至极致。

核心设计原则：**生产者热路径零格式化、零系统调用、零锁竞争；时间戳归并、文本解码与内核 I/O 全部异步下沉至消费端执行**。

---

## 为什么需要 zerolog？

在超低延迟 C++ 系统（低延迟分布式存储、RPC 网关、网络数据面）中，关键路径打日志往往受制于**三大根本瓶颈**：

1. **系统调用与操作系统时钟开销**
   - *痛点*：传统日志库在调用线程直接调用 `clock_gettime`（`std::chrono::system_clock::now()`）获取墙上时钟，并在业务线程触发同步 `write` 系统调用或页缓存锁，带来微秒级尾延迟抖动。
   - *`zerolog` 的解法*：生产者在热路径完全绕过系统调用，直接读取 CPU 硬件时间戳计数器（`x86_64` `rdtsc`）。消费者通过标定后的仿射乘法移位（`TscClock`）将硬件 tick 换算为纳秒时间戳，并复用秒级头缓存（`HeaderCache`），使稳态日志头渲染执行**零次整数除法（`div` / `idiv`）**；所有内核写入与文件轮转均由 `uring` `io_uring` Proactor（或 `futex2` 回退后端）异步完成。

2. **热路径字符串格式化与运行期格式串解析**
   - *痛点*：在业务线程上通过 `std::format`、`snprintf` 或 `std::ostream` 将整数、浮点数、枚举与业务结构体格式化为字符串，包含大量分支与除法指令，严重污染 L1i/L1d 缓存并抢占核心算力。
   - *`zerolog` 的解法*：**热路径零格式化。** 格式串在编译期（`consteval`）完成校验与切分，并通过 **C++26 P3491（`std::define_static_array` / `std::define_static_string`）** 提升至只读静态段。调用线程仅做纯二进制数据搬运——将 8 字节 `StaticRecordInfo*` 描述符指针、TSC 时间戳、Span ID 及参数原始字节（`Codec<T>`）拷入环形缓冲区，将所有文本拼装、浮点/十六进制渲染与日志头生成完全卸载到消费端。

3. **堆内存分配、跨核锁竞争与线程栈膨胀**
   - *痛点*：共享 `std::mutex` 或 MPSC 队列在突发流量下会产生剧烈的跨核 CAS 缓存行乒乓；动态分配 `std::string` 带来分配器抖动；而沉重的 `io_uring` 消费者状态又容易撑爆小栈线程（如 `128 KiB` musl 线程栈）。
   - *`zerolog` 的解法*：每个生产者线程独占一块 **2 MiB 透明大页（THP）无锁变长 SPSC 环形缓冲区（`SpscVarQueue`）**，由 64 槽位无锁 `ThreadRegistry` 配合基于纪元（epoch）的孤儿回收机制管理。热路径预留（`TryReserve`）仅读取线程本地额度窗口：**零原子 RMW、零栈帧（`Deny::Stack`）、零函数调用指令（`Deny::Call`）、指令数 `≤ 64` 条**。消费端通过 8 路锦标赛树按 TSC 时间戳归并最多 64 个生产者队列，格式化进双 16 MiB `MAP_NORESERVE` 缓冲区（`OutputBuffer`），支持挂载已有 `uring::Proactor`（模式 A）、独立 512 KiB 栈工作线程 `UringConsumerThread`（模式 B）或同步 `Poll`（模式 C）。

### 零样板代码的 C++26 现代体验

在底层二进制 SPSC + `io_uring` 架构之上，`zerolog` 提供了符合直觉、无需宏侵入的 C++26 API：
- **即发即忘的日志调用**（`zerolog::Trace`、`Debug`、`Info`、`Warn`、`Error`、`Fatal`、`Log<Level>`）：返回 `void`，业务代码无需为每一行日志编写返回值检查样板。
- **显式错误感知的 `Try*` 族接口**（`zerolog::TryTrace`、`TryDebug`、`TryInfo`、`TryWarn`、`TryError`、`TryFatal`、`TryLog<Level>`）：返回 `[[nodiscard]] ErrorCode`，需要对背压或超限错误做精确处理时可直接获取真实错误码——全库绝不静默截断、夹逼或调用 `abort()`。
- **C++26 P2996 静态反射（`^^T`）**：`Codec<T>` 自动反射枚举名与聚合结构体字段（输出 `{field=value, ...}`），无需派生宏或手写 `formatter` 特化。
- **编译期格式说明符**：基于 **P0843 `InplaceVector`** 在编译期解析 `{}`、`{:x}`、`{:#X}`、`{:b}`、`{:#b}`、`{:o}`、`{:.3}`、`{:>10}`、`{:<10}`、`{:^10}`、`{:08}`。
- **链式 `Builder` + RAII `LoggerGuard`**：一键启动后台消费线程，作用域结束析构时自动停止工作线程并刷出全部未落盘日志。

---

## 核心特性

- **无锁生产者热路径** — `≤ 64` 条机器指令、零栈帧、零 `call` 指令（冷路径经尾跳转进入 `.text.unlikely`）、零 `div`/`idiv`/`movabs`。
- **零开销级别过滤** — 单次 `relaxed` 原子读取 32 位打包 `LevelGateWord`。
- **C++26 P2996 静态反射** — 自动对枚举与聚合结构体执行二进制快照编码并在消费端还原字段名与枚举项名。
- **C++26 P3491 静态提升** — `std::define_static_array` / `std::define_static_string` 仅将有效格式片段提升至静态存储。
- **无除法时间戳与数字格式化** — `TscClock` 仿射乘法移位换算、1 秒级 `HeaderCache`、双字符查表数字渲染，零 `<format>` / `libfmt` 运行时依赖。
- **三种消费运行模式**：
  - **模式 A（`ProactorLogSink`）**：作为 Sans-IO 子系统挂载到应用已有的 `uring::Proactor` 事件循环。
  - **模式 B（`Builder` / `UringConsumerThread`）**：独立 512 KiB 栈后台线程，当内核禁用 `io_uring` 时自动降级至 `futex2` 后端。
  - **模式 C（`Poll` / `PollBatch`）**：调用方线程同步排空与刷盘（输出至文件描述符或 16 字节 `WriteCallback` 委托）。
- **全程 `-fno-exceptions -fno-rtti` 构建** — 所有目标文件零异常表（无 `.gcc_except_table` / `__cxa_throw`）。

---

## 快速上手

### 最小示例（通过 `Builder` + RAII `LoggerGuard` 启动后台消费线程）

```cpp
#include <chrono>
#include <zerolog/zerolog.hpp>

int main() {
  // 启动后台消费线程 —— guard 离开作用域时自动停止线程并刷出所有挂起日志
  auto guard = zerolog::Builder()
                   .Level(zerolog::kInfo)
                   .IdleTimeout(std::chrono::milliseconds(1))
                   .Start();
  if (!guard) return 1;

  zerolog::Info("server started on port {}", 8080U);
  zerolog::Warn("connection pool at {}% capacity", 85U);
  zerolog::Error("request failed with code {:#x}", 0xDEADU);

  return 0;
}
```

输出示例：

```text
2026-11-07 12:00:00.123456789 [INF] [tid:0] [span:0] main.cc:13: server started on port 8080
2026-11-07 12:00:00.123457012 [WRN] [tid:0] [span:0] main.cc:14: connection pool at 85% capacity
2026-11-07 12:00:00.123457234 [ERR] [tid:0] [span:0] main.cc:15: request failed with code 0xDEAD
```

---

## 日志 API

### 日常无返回值日志接口（`void`）

日常日志调用返回 `void`，首次调用时自动为当前线程注册 2 MiB SPSC 环形缓冲区（也可在线程启动时调用 `zerolog::PrewarmThread()` 提前预热）：

| 函数 | 级别 | 示例 |
|---|---|---|
| `zerolog::Trace(fmt, ...)` | `kTrace` | `zerolog::Trace("poll state={}", state);` |
| `zerolog::Debug(fmt, ...)` | `kDebug` | `zerolog::Debug("value={}", x);` |
| `zerolog::Info(fmt, ...)` | `kInfo` | `zerolog::Info("started on port {}", 8080U);` |
| `zerolog::Warn(fmt, ...)` | `kWarn` | `zerolog::Warn("slow query: {}ms", elapsed);` |
| `zerolog::Error(fmt, ...)` | `kError` | `zerolog::Error("failed: {}", code);` |
| `zerolog::Fatal(fmt, ...)` | `kError` | `zerolog::Fatal("unrecoverable state={}", st);` |
| `zerolog::Log<Level>(fmt, ...)` | 任意编译期 `Level` | `zerolog::Log<zerolog::kInfo>("custom={}", v);` |

### 显式返回错误码的 `Try*` 接口

越界输入、环满、短写与文件轮转失败绝不会静默截断、伪造占位符或触发 `abort()`。当调用方需要显式感知 `ErrorCode`（`kSuccess`、`kQueueFull`、`kRecordTooLarge`、`kConsumerBusy`、`kWriteFailed`、`kShuttingDown`、`kTooManyThreads`）时，可调用 `Try*` 系列接口：

```cpp
if (const auto err = zerolog::TryInfo("request id={}", request_id); err != zerolog::ErrorCode::kSuccess) {
  // 显式处理队列满背压 (kQueueFull) 或超长记录 (kRecordTooLarge)
}

if (auto stats = zerolog::TryPoll(true); !stats) {
  return stats.error();
}
```

### 惰性求值与按级别执行（`Lazy` & `IfEnabled`）

使用 `zerolog::Lazy` 包装高开销参数计算，仅在该条日志通过级别过滤时才会求值：

```cpp
zerolog::Debug("snapshot={}", zerolog::Lazy{[&]() noexcept { return BuildSnapshotSummary(); }});

zerolog::IfEnabled(zerolog::kDebug, [&]() noexcept {
  zerolog::Debug("detailed state={}", ComputeDiagnostics());
});
```

---

## 格式说明符

格式串在编译期（`consteval`）严格校验，花括号不匹配、非法格式符或参数数量不符会在编译期直接报错：

```cpp
zerolog::Info("decimal:    {}", 255U);         // 255
zerolog::Info("hex:        {:x}", 255U);       // ff
zerolog::Info("upper hex:  {:#X}", 255U);      // 0xFF
zerolog::Info("binary:     {:#b}", 10U);       // 0b1010
zerolog::Info("octal:      {:o}", 255U);       // 377
zerolog::Info("precision:  {:.3}", 3.14159);   // 3.142
zerolog::Info("right-pad:  {:>10}", 42U);      //         42
zerolog::Info("left-pad:   {:<10}", 42U);      // 42
zerolog::Info("center:     {:^10}", 42U);      //     42
zerolog::Info("zero-pad:   {:08}", 42U);       // 00000042
```

---

## 基于 C++26 P2996 静态反射的自定义类型

任何 C++26 `enum` 或成员可编码的标准布局聚合 `struct`，均可通过 C++26 静态反射（`^^T` 与 `std::meta::nonstatic_data_members_of`）直接打印——**无需注册宏、无需手写序列化模板**：

```cpp
enum class Direction : std::uint8_t { kIngress = 1, kEgress = 2 };

struct Request {
  std::uint64_t id;
  double latency_us;
  std::uint32_t bytes;
  Direction dir;
};

zerolog::Info("new request: {}", Request{.id = 1001, .latency_us = 49.99, .bytes = 100, .dir = Direction::kIngress});
// → new request: {id=1001, latency_us=49.99, bytes=100, dir=kIngress}
```

在热路径上，`Codec<Request>` 直接将定长成员字节拷入 SPSC 环；字段名与枚举项名在编译期提取，并在后台消费线程完成格式化。

---

## Builder 配置与三种消费模式

### 模式 B：独立后台消费线程（`Builder` / `UringConsumerThread`）

[`examples/standalone_worker.cc`](examples/standalone_worker.cc)

`Builder` 负责配置日志级别、文件轮转与队列策略，并在独立的 512 KiB 栈线程上启动 `UringConsumerThread`（即使调用方位于 128 KiB 栈的 musl 线程上也完全安全）。当容器或内核禁用 `io_uring_setup` 时，工作线程自动切换至 `FutexFallbackSink` 并通过 `Backend()` 报告 `WorkerBackend::kFutexFallback`：

```cpp
#include <chrono>
#include <zerolog/zerolog.hpp>

int main() {
  auto guard = zerolog::Builder()
                   .Level(zerolog::kInfo)
                   .LogFile("/var/log/app.log")
                   .Rotation({.max_bytes = 100U * 1024U * 1024U, .max_files = 5U})
                   .FlushOn(zerolog::kError)
                   .QueueFullPolicy(zerolog::QueueFullPolicy::kDrop)
                   .IdleTimeout(std::chrono::milliseconds(1))
                   .Start();
  if (!guard) return static_cast<int>(guard.error());

  zerolog::SetThreadName("worker-0");
  zerolog::Info("standalone worker ready value={}", 42U);

  if (auto flushed = guard->Shutdown(); !flushed) {
    return static_cast<int>(flushed.error());
  }
  return 0;
}
```

### 模式 A：共享 `uring::Proactor` 子系统（`ProactorLogSink`）

[`examples/shared_proactor.cc`](examples/shared_proactor.cc)

当应用自身已经在驱动单核单线程 `uring::Proactor` 事件循环时，可将 `ProactorLogSink` 作为 `uring::SingleDispatcher` 子系统直接挂入现有环中，零额外后台线程开销：

```cpp
zerolog::Rotation rotation{};
auto opened = rotation.Open(path, {.max_bytes = 64U * 1024U, .max_files = 2U});
auto consumer = zerolog::AcquireConsumer();
auto engine = uring::Proactor::Create(64U, uring::RingMode::kStandard64_16,
                                      zerolog::ProactorLogSink<>::RequiredCaps());

zerolog::ProactorLogSink<> sink{*engine, *consumer, rotation};
uring::SingleDispatcher dispatcher{sink};
engine->BindDispatcher(dispatcher);
[[maybe_unused]] auto started = sink.Start();

zerolog::Info("shared proactor ready value={}", 7U);
```

### 模式 C：同步手动排空（`Poll` / `PollBatch` 与 `WriteCallback`）

[`examples/manual_poll.cc`](examples/manual_poll.cc)

在确定性单元测试、命令行工具或自定义轮询循环中，可直接调用 `zerolog::Poll(true)` 同步排空并写出，或通过 `uarch::bind` 绑定 16 字节 `zerolog::WriteCallback`（`uarch::Callback<Result<std::uint32_t>(std::span<const char>)>`）：

```cpp
#include <span>
#include <string>
#include <uarch/core/flow/delegate.hpp>
#include <zerolog/zerolog.hpp>

struct BufferSink {
  std::string captured;
  zerolog::Result<std::uint32_t> Write(std::span<const char> bytes) noexcept {
    captured.append(bytes.data(), bytes.size());
    return static_cast<std::uint32_t>(bytes.size());
  }
};

int main() {
  zerolog::PrewarmThread();
  BufferSink sink{};
  if (auto consumer = zerolog::AcquireConsumer()) {
    [[maybe_unused]] auto writer_set = consumer->SetWriter(uarch::bind<&BufferSink::Write>(sink));
    zerolog::Info("manual poll ready");
    [[maybe_unused]] auto polled = consumer->Poll(true);
  }
  return 0;
}
```

---

## 多线程并发、Span 上下文与文件轮转

[`tests/e2e/pipeline_e2e_test.cc`](tests/e2e/pipeline_e2e_test.cc)

每个生产者线程拥有独立的 SPSC 环形缓冲区，互不争抢多生产者锁：

```cpp
zerolog::SetThreadName("worker-0");
const zerolog::SpanGuard span{101U};
zerolog::Info("lane={} sample={}", lane, sample);
```

配置按大小轮转（`RotationPolicy{.max_bytes = ..., .max_files = N}`）后，当活动文件达到阈值时，消费端自动通过异步 `io_uring` `UNLINKAT` / `RENAMEAT` / `OPENAT` / `CLOSE` SQE（或回退路径同步系统调用）完成 `app.log` → `app.log.1` → ... → `app.log.N` 滚动。

---

## 架构与数据流

```text
Producer Thread 0 ──→ [2 MiB THP SPSC Ring 0] ──┐
Producer Thread 1 ──→ [2 MiB THP SPSC Ring 1] ──┤
...                                             ├──→ 8 路锦标赛树按 TSC 归并 (ConsumerCore)
Producer Thread 63 ─→ [2 MiB THP SPSC Ring 63] ─┘      │
                                                       ▼
                                         ┌─── 双 16 MiB OutputBuffer ────┐
                                         │ 1. 仿射 TscClock 纳秒换算       │
                                         │ 2. 秒级 HeaderCache 命中       │
                                         │ 3. P2996/P3491 静态解码器      │
                                         │ 4. 模式 A: 共享 Proactor       │
                                         │    模式 B: UringConsumerThread │
                                         │    模式 C: 同步 Poll           │
                                         └────────────────────────────────┘
```

**生产者热路径**（已预热标量记录）：
1. **级别门控检查** — 单次 `relaxed` 原子读取 `LevelGateWord`。
2. **TLS 缓冲区读取** — 直接读取 `t_buffer`（未预热线程首次调用走冷路径自动注册）。
3. **SPSC `TryReserve`** — 检查线程本地缓存额度并推进写指针，快速路径零原子 RMW。
4. **二进制快照写入** — 写入 `StaticRecordInfo*`、`ReadTsc()`、`CurrentSpanId()` 与参数编码字节。
5. **Release 发布** — 单次 `release` 原子写入 `RecordHeaderWord` + 编译器信号栅栏。
6. **条件门铃通知** — 稳态静默返回；仅当越过紧急水位线或达到 `FlushOn` 级别时尾跳转至冷路径唤醒消费者。

---

## 命令行与门禁验证

`./zerolog` 是工程构建与微架构门禁驱动脚本，`--preset` 默认使用 `clang-glibc-release`，矩阵涵盖 `clang-glibc-release`、`gcc-glibc-release`、`clang-musl-release`、`gcc-musl-release`、`clang-glibc-asan-ubsan` 与 `gcc-glibc-asan-ubsan`。

| 命令 | 说明 |
|---|---|
| `./zerolog setup` | 配置指定 CMake 预设。 |
| `./zerolog build` | 配置并构建全部目标。 |
| `./zerolog test` | 构建并运行全部 57 项 CTest 测试套件（单元、编译期、编译失败、代码生成与端到端测试）。 |
| `./zerolog format --check` | 检查所有头文件与源文件的 `clang-format` 格式规范。 |
| `./zerolog tidy` | 基于编译数据库运行 `clang-tidy` 静态检查。 |
| `./zerolog verify --tier 1` | 构建、运行测试，并通过 `llvm-nm`、`llvm-readobj`、`llvm-objdump`、`llvm-size`、`llvm-mca` 审计探针目标文件。 |
| `./zerolog verify --tier 2` | 运行 Tier 1 门禁，并执行 `gem5.opt` O3CPU 三点差分乱序流水线仿真与 `AMDuProfPcm` 硬件 TMA 剖析。 |
