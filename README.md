# zerolog

[中文说明](README.zh-CN.md)

A C++26 structured logging library with per-thread lock-free SPSC huge-page rings, compile-time P2996/P3491 reflection, and asynchronous `io_uring` / `futex2` consumer formatting for ultra-low hot-path producer overhead.

Core design principle: **zero formatting, zero syscalls, and zero locks on the producer hot path; asynchronous timestamp merge, decoding, and kernel I/O on the consumer side**.

---

## Why zerolog?

In ultra-low-latency C++ systems (distributed storage engines, RPC gateways, network data planes), logging on the critical path typically fails because of **three fundamental bottlenecks**:

1. **System Calls & OS Clock Overhead**
   - *The Problem*: Traditional C++ loggers query wall-clock time via `clock_gettime` (`std::chrono::system_clock::now()`) and issue kernel `write` syscalls or page-cache locks directly on the caller thread, introducing microsecond-level jitter.
   - *How `zerolog` solves it*: Producers bypass syscalls completely on the hot path by reading the CPU hardware timestamp counter directly (`rdtsc` on `x86_64`). The consumer converts hardware ticks into wall-clock nanoseconds using a calibrated affine multiplier (`TscClock`) and reuses a per-second `HeaderCache` so steady-state timestamp rendering executes **zero integer divisions (`div` / `idiv`)**. Kernel I/O is offloaded to a `uring` `io_uring` Proactor (or `futex2` fallback).

2. **Hot-Path String Formatting & Runtime Format Parsing**
   - *The Problem*: Formatting integers, floats, enums, and domain structs into text strings via `std::format`, `snprintf`, or `std::ostream` on a critical worker thread is branch-heavy, division-heavy, and pollutes L1i/L1d caches.
   - *How `zerolog` solves it*: **Zero formatting on the hot path.** Format strings are validated and segmented at compile time (`consteval`) and promoted into `.rodata` via **C++26 P3491 (`std::define_static_array` / `std::define_static_string`)**. The caller thread only performs raw binary data movement—copying an 8-byte `StaticRecordInfo*` descriptor pointer, hardware TSC timestamp, span ID, and binary argument bytes (`Codec<T>`) into its ring buffer—and offloads all string formatting, float/hex rendering, and header assembly to the consumer.

3. **Heap Allocations, Cross-Thread Contention & Stack Explosion**
   - *The Problem*: Shared `std::mutex` writers or MPSC queues suffer cross-core CAS contention under burst load; dynamic `std::string` allocations introduce allocator jitter; and heavy `io_uring` consumer state can overflow small (`128 KiB` musl) thread stacks.
   - *How `zerolog` solves it*: Every producer thread owns a dedicated **2 MiB Transparent Huge Page (THP) lock-free SPSC ring buffer (`SpscVarQueue`)** managed by a 64-slot lock-free `ThreadRegistry` with epoch-based orphan reclamation. Fast-path reservations (`TryReserve`) use a thread-local credit window: **zero atomic RMWs, zero stack frame (`Deny::Stack`), zero `call` instructions (`Deny::Call`), and `≤ 64` native instructions**. On the consumer side, an 8-way tournament tree merges up to 64 producer rings by TSC timestamp into dual 16 MiB `MAP_NORESERVE` slabs (`OutputBuffer`), driven by a shared `uring::Proactor` (Mode A), a standalone 512 KiB-stack `UringConsumerThread` (Mode B), or synchronous `Poll` (Mode C).

### Zero-Boilerplate C++26 Ergonomics

Despite its binary SPSC + `io_uring` architecture under the hood, `zerolog` provides an idiomatic, zero-macro C++26 API surface:
- **Fire-and-forget logging functions** (`zerolog::Trace`, `Debug`, `Info`, `Warn`, `Error`, `Fatal`, `Log<Level>`) that return `void` and never force return-value boilerplate on business logic.
- **Explicit `Try*` diagnostic counterparts** (`zerolog::TryTrace`, `TryDebug`, `TryInfo`, `TryWarn`, `TryError`, `TryFatal`, `TryLog<Level>`) returning `[[nodiscard]] ErrorCode` when backpressure or error inspection is desired—never silently truncating, clamping, or calling `abort()` on out-of-range inputs.
- **C++26 P2996 Static Reflection (`^^T`)** in `Codec<T>` that automatically logs `enum class` names and aggregate `struct` fields (`{field=value, ...}`) without `#[derive]` macros or manual `formatter` specializations.
- **Compile-time format specifiers** (`{}`, `{:x}`, `{:#X}`, `{:b}`, `{:#b}`, `{:o}`, `{:.3}`, `{:>10}`, `{:<10}`, `{:^10}`, `{:08}`) parsed in `consteval` using **P0843 `InplaceVector`**.
- **Fluent `Builder` + RAII `LoggerGuard`** that starts the background consumer thread and automatically flushes all pending records on scope exit.

---

## Highlights

- **Lock-free producer hot path** — `≤ 64` instructions, zero stack frame, zero `call` instructions (cold paths tail-jump to `.text.unlikely`), zero `div`/`idiv`/`movabs`.
- **Zero-cost level filtering** — a single `relaxed` atomic load of the packed 32-bit `LevelGateWord`.
- **C++26 P2996 Reflection** — automatic binary snapshot encoding and consumer-side field/enum rendering for user enums and aggregate structs.
- **C++26 P3491 Static Promotion** — `std::define_static_array` / `std::define_static_string` promote only live format segments into static storage.
- **Division-free timestamp & number formatting** — `TscClock` affine mul-shift conversion, 1-second `HeaderCache`, 2-digit lookup tables, and zero `<format>` / `libfmt` runtime dependency.
- **Three Consumer Execution Modes**:
  - **Mode A (`ProactorLogSink`)**: attaches as a Sans-IO subsystem onto an existing `uring::Proactor`.
  - **Mode B (`Builder` / `UringConsumerThread`)**: dedicated 512 KiB-stack background thread with automatic `futex2` fallback when the kernel restricts `io_uring`.
  - **Mode C (`Poll` / `PollBatch`)**: caller-driven synchronous drain and flush (to a file descriptor or a 16-byte `WriteCallback` delegate).
- **Built with `-fno-exceptions` and `-fno-rtti`** — zero exception tables (`.gcc_except_table` / `__cxa_throw`) across all objects.

---

## Quick Start

### Minimal Example (Background Worker via `Builder` + RAII `LoggerGuard`)

```cpp
#include <chrono>
#include <zerolog/zerolog.hpp>

int main() {
  // Start the background consumer thread — guard auto-stops and flushes on scope exit
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

Output:

```text
2026-11-07 12:00:00.123456789 [INF] [tid:0] [span:0] main.cc:13: server started on port 8080
2026-11-07 12:00:00.123457012 [WRN] [tid:0] [span:0] main.cc:14: connection pool at 85% capacity
2026-11-07 12:00:00.123457234 [ERR] [tid:0] [span:0] main.cc:15: request failed with code 0xDEAD
```

---

## Logging API

### Fire-and-Forget Logging (`void`)

Everyday logging calls return `void` and automatically register the calling thread's 2 MiB SPSC ring buffer on first use (or use the prewarmed buffer if `zerolog::PrewarmThread()` was called at thread startup):

| Function | Level | Example |
|---|---|---|
| `zerolog::Trace(fmt, ...)` | `kTrace` | `zerolog::Trace("poll state={}", state);` |
| `zerolog::Debug(fmt, ...)` | `kDebug` | `zerolog::Debug("value={}", x);` |
| `zerolog::Info(fmt, ...)` | `kInfo` | `zerolog::Info("started on port {}", 8080U);` |
| `zerolog::Warn(fmt, ...)` | `kWarn` | `zerolog::Warn("slow query: {}ms", elapsed);` |
| `zerolog::Error(fmt, ...)` | `kError` | `zerolog::Error("failed: {}", code);` |
| `zerolog::Fatal(fmt, ...)` | `kError` | `zerolog::Fatal("unrecoverable state={}", st);` |
| `zerolog::Log<Level>(fmt, ...)` | Any compile-time `Level` | `zerolog::Log<zerolog::kInfo>("custom={}", v);` |

### Explicit Error-Reporting Counterparts (`Try*`)

Out-of-range inputs, full rings, short writes, and failed rotations are never silently clamped, replaced with fake truncation markers, or turned into `abort()`. When a caller wants to inspect the exact `ErrorCode` (`kSuccess`, `kQueueFull`, `kRecordTooLarge`, `kConsumerBusy`, `kWriteFailed`, `kShuttingDown`, `kTooManyThreads`), use the `Try*` family:

```cpp
if (const auto err = zerolog::TryInfo("request id={}", request_id); err != zerolog::ErrorCode::kSuccess) {
  // Handle backpressure (kQueueFull) or oversized payload (kRecordTooLarge)
}

if (auto stats = zerolog::TryPoll(true); !stats) {
  return stats.error();
}
```

### Deferred Argument Evaluation (`Lazy` & `IfEnabled`)

Wrap expensive argument computations in `zerolog::Lazy` so they are evaluated **only** when the log level is enabled:

```cpp
zerolog::Debug("snapshot={}", zerolog::Lazy{[&]() noexcept { return BuildSnapshotSummary(); }});

zerolog::IfEnabled(zerolog::kDebug, [&]() noexcept {
  zerolog::Debug("detailed state={}", ComputeDiagnostics());
});
```

---

## Format Specifiers

Format strings are checked at compile time (`consteval`). Malformed braces, unknown specifiers, or argument count mismatches fail compilation immediately:

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

## Custom Types via C++26 P2996 Reflection

Any C++26 `enum` or standard-layout aggregate `struct` with encodeable members is logged automatically via C++26 reflection (`^^T` and `std::meta::nonstatic_data_members_of`)—**zero macros and zero boilerplate required**:

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

On the hot path, `Codec<Request>` copies the fixed-size member bytes directly into the SPSC ring buffer. Enumerator and field names are resolved at compile time and formatted on the consumer thread.

---

## Builder Configuration & Consumer Modes

### Mode B: Standalone Background Worker (`Builder` / `UringConsumerThread`)

[`examples/standalone_worker.cc`](examples/standalone_worker.cc)

`Builder` configures the logger and spawns `UringConsumerThread` on a dedicated 512 KiB stack (keeping caller stack usage safe even on 128 KiB musl threads). When `io_uring_setup` is restricted by container seccomp or kernel policy, the worker automatically falls back to `FutexFallbackSink` and reports `WorkerBackend::kFutexFallback`:

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

### Mode A: Shared `uring::Proactor` Subsystem (`ProactorLogSink`)

[`examples/shared_proactor.cc`](examples/shared_proactor.cc)

When an application already drives a single-thread-per-core `uring::Proactor` event loop, `ProactorLogSink` attaches directly as a `uring::SingleDispatcher` subsystem with zero extra threads:

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

### Mode C: Synchronous Manual Poll (`Poll` / `PollBatch` & `WriteCallback`)

[`examples/manual_poll.cc`](examples/manual_poll.cc)

For deterministic tests, CLI tools, or custom polling loops, call `zerolog::Poll(true)` directly on any thread, or attach a 16-byte `zerolog::WriteCallback` (`uarch::Callback<Result<std::uint32_t>(std::span<const char>)>`) via `uarch::bind`:

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

## Multi-threaded Logging, Spans, and Rotation

[`tests/e2e/pipeline_e2e_test.cc`](tests/e2e/pipeline_e2e_test.cc)

Each producer thread writes into its own lock-free SPSC ring buffer without cross-producer contention:

```cpp
zerolog::SetThreadName("worker-0");
const zerolog::SpanGuard span{101U};
zerolog::Info("lane={} sample={}", lane, sample);
```

When size-based rotation (`RotationPolicy{.max_bytes = ..., .max_files = N}`) triggers, archives shift downward (`app.log` → `app.log.1` → ... → `app.log.N`) via asynchronous `io_uring` `UNLINKAT` / `RENAMEAT` / `OPENAT` / `CLOSE` SQEs (or synchronous syscalls in `FutexFallbackSink` / Mode C).

---

## Architecture

```text
Producer Thread 0 ──→ [2 MiB THP SPSC Ring 0] ──┐
Producer Thread 1 ──→ [2 MiB THP SPSC Ring 1] ──┤
...                                             ├──→ 8-Way Tournament Merge (ConsumerCore)
Producer Thread 63 ─→ [2 MiB THP SPSC Ring 63] ─┘      │
                                                       ▼
                                         ┌─── Dual 16 MiB OutputBuffer ───┐
                                         │ 1. Affine TscClock ns mapping  │
                                         │ 2. Per-second HeaderCache hit  │
                                         │ 3. P2996/P3491 static decoder  │
                                         │ 4. Mode A: Shared Proactor     │
                                         │    Mode B: UringConsumerThread │
                                         │    Mode C: Synchronous Poll    │
                                         └────────────────────────────────┘
```

**Producer hot path** (warmed scalar logging):
1. **Level gate check** — single `relaxed` atomic load (`LevelGateWord`).
2. **TLS buffer lookup** — reads `t_buffer` (cold registration on first call if not prewarmed).
3. **SPSC `TryReserve`** — local credit check and pointer advance; zero atomic RMWs on fast path.
4. **Binary snapshot write** — stores `StaticRecordInfo*`, `ReadTsc()`, `CurrentSpanId()`, and encoded argument bytes.
5. **Release publication** — single `release` store of `RecordHeaderWord` + compiler signal fence.
6. **Conditional doorbell** — stays silent in steady state; tail-jumps to cold futex2 wake only on urgent watermark or `FlushOn` threshold.

---

## CLI & Verification Gates

`./zerolog` is the project build and verification driver. `--preset` defaults to `clang-glibc-release`. The matrix includes `clang-glibc-release`, `gcc-glibc-release`, `clang-musl-release`, `gcc-musl-release`, `clang-glibc-asan-ubsan`, and `gcc-glibc-asan-ubsan`.

| Command | Description |
|---|---|
| `./zerolog setup` | Configure the CMake preset. |
| `./zerolog build` | Configure and build all targets. |
| `./zerolog test` | Build and run the 57 CTest suites (unit, consteval, compile-fail, codegen, and E2E). |
| `./zerolog format --check` | Verify `clang-format` compliance across all headers and sources. |
| `./zerolog tidy` | Run `clang-tidy` across the compilation database. |
| `./zerolog verify --tier 1` | Build, run all tests, and audit native probe objects via `llvm-nm`, `llvm-readobj`, `llvm-objdump`, `llvm-size`, and `llvm-mca`. |
| `./zerolog verify --tier 2` | Run Tier 1 gates plus 3-point `gem5.opt` O3CPU linearity simulation and `AMDuProfPcm` hardware TMA profiling. |
