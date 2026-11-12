#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <zerolog/encode/pack_encoder.hpp>
#include <zerolog/format/header_cache.hpp>

namespace {
// m5_reset_stats(ns_delay, ns_period) and m5_dump_stats read rdi then rsi.
// A leftover non-zero delay postpones the ROI, so the dump records numCycles=1.
// The installed uarch headers do not declare the ROI entry points, so the opcodes live here.
sigjmp_buf g_roi_jump;
volatile std::sig_atomic_t g_roi_tripped = 0;
bool g_roi_gem5 = false;
constexpr std::uint64_t kWarmupIterations = 8;
constexpr std::int64_t kOriginNs = 1'700'000'000'000'000'000;

void OnSigill(int) {
  g_roi_tripped = 1;
  siglongjmp(g_roi_jump, 1);
}

void EmitReset() noexcept {
  asm volatile(
      "xor %%edi, %%edi\n\t"
      "xor %%esi, %%esi\n\t"
      ".byte 0x0f, 0x04\n\t"
      ".word 0x0040\n\t"
      :
      :
      : "rdi", "rsi", "cc", "memory");
}

void EmitDump() noexcept {
  asm volatile(
      "xor %%edi, %%edi\n\t"
      "xor %%esi, %%esi\n\t"
      ".byte 0x0f, 0x04\n\t"
      ".word 0x0041\n\t"
      :
      :
      : "rdi", "rsi", "cc", "memory");
}

[[nodiscard]] bool ProbeGem5() noexcept {
#if !defined(__x86_64__)
  return false;
#else
  struct sigaction next{};
  struct sigaction previous{};
  next.sa_handler = OnSigill;
  if (sigemptyset(&next.sa_mask) != 0 || sigaction(SIGILL, &next, &previous) != 0) return false;
  g_roi_tripped = 0;
  if (sigsetjmp(g_roi_jump, 1) == 0) EmitReset();
  sigaction(SIGILL, &previous, nullptr);
  return g_roi_tripped == 0;
#endif
}

void DoNotOptimize(std::uint64_t& value) noexcept { asm volatile("" : "+r"(value) : : "memory"); }

// The cold iterations stay outside the ROI. HeaderCache and the stack slots they touch
// would otherwise enter the timed window and show up as a constant memory-order count.
[[gnu::noinline]] [[nodiscard]] zerolog::ErrorCode RunProbe(zerolog::format::HeaderCache& cache, std::uint64_t iterations,
                                                            std::uint64_t& checksum) noexcept {
  for (std::uint64_t i = 0; i < iterations; ++i) {
    alignas(std::uint64_t) std::byte payload[16];
    const auto encoded = zerolog::encode::EncodeRecord(payload, sizeof(payload), i * 17U, i * 31U);
    if (encoded != zerolog::ErrorCode::kSuccess) return encoded;
    char text[96];
    std::size_t length = 0;
    zerolog::abi::OutputAppender out{text, &length, sizeof(text)};
    const auto formatted = cache.Append(out, kOriginNs + static_cast<std::int64_t>(i % 500U), zerolog::abi::Level::kInfo, 1, 0);
    if (formatted != zerolog::ErrorCode::kSuccess) return formatted;
    checksum ^= payload[0] == std::byte{0} ? 0U : 1U;
    checksum += length;
    DoNotOptimize(checksum);
  }
  return zerolog::ErrorCode::kSuccess;
}

[[nodiscard]] bool TenDigits(const char* text, std::uint64_t& value) noexcept {
  if (text == nullptr || std::strlen(text) != 10) return false;
  value = 0;
  for (int i = 0; i < 10; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10U + static_cast<std::uint64_t>(text[i] - '0');
  }
  return value == 512 || value == 2560 || value == 4608 || value == 5000000;
}

[[nodiscard]] bool ParsePositive(const char* text, std::uint64_t& value) noexcept {
  if (text == nullptr || text[0] == '\0') return false;
  value = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const auto digit = static_cast<std::uint64_t>(*cursor - '0');
    if (value > (UINT64_MAX - digit) / 10U) return false;
    value = value * 10U + digit;
  }
  return value != 0;
}

[[nodiscard]] const char* ArgValue(int argc, char** argv, const char* flag) noexcept {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
  }
  return nullptr;
}

void RoiBegin() noexcept {
  if (g_roi_gem5) EmitReset();
}

void RoiEnd() noexcept {
  if (g_roi_gem5) EmitDump();
}
}  // namespace

int main(int argc, char** argv) {
  const char* probe = ArgValue(argc, argv, "--probe");
  const char* marker = ArgValue(argc, argv, "--iterations");
  const char* bench = ArgValue(argc, argv, "--bench-iterations");
  if (probe == nullptr || std::strcmp(probe, "encode_u64x2") != 0 || (marker == nullptr) == (bench == nullptr)) {
    std::fputs(
        "zerolog_gem5_harness: expected --probe encode_u64x2 and either --iterations <10-digit marker> "
        "or --bench-iterations <count>\n",
        stderr);
    return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
  }
  std::uint64_t iterations = 0;
  const char* printed = marker;
  if (bench != nullptr) {
    printed = bench;
    if (!ParsePositive(bench, iterations)) {
      std::fputs("zerolog_gem5_harness: --bench-iterations must be a positive decimal integer\n", stderr);
      return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
    }
  } else if (!TenDigits(marker, iterations)) {
    std::fputs("zerolog_gem5_harness: iterations must be 0000000512, 0000002560, 0000004608, or 0005000000\n", stderr);
    return static_cast<int>(zerolog::ErrorCode::kInvalidArgument);
  }

  g_roi_gem5 = ProbeGem5();
  zerolog::format::HeaderCache cache;
  std::uint64_t warm_checksum = 0;
  const auto warmed = RunProbe(cache, kWarmupIterations, warm_checksum);
  if (warmed != zerolog::ErrorCode::kSuccess) return static_cast<int>(warmed);
  asm volatile("" : "+r"(warm_checksum) : : "memory");

  RoiBegin();
  std::uint64_t checksum = 0;
  const auto ran = RunProbe(cache, iterations, checksum);
  RoiEnd();
  asm volatile("" : "+r"(checksum) : : "memory");
  if (ran != zerolog::ErrorCode::kSuccess) return static_cast<int>(ran);

  std::printf("ZEROLOG_GEM5 probe=%s n=%s checksum=%llu roi=%s\n", probe, printed, static_cast<unsigned long long>(checksum),
              g_roi_gem5 ? "gem5" : "host");
  return static_cast<int>(zerolog::ErrorCode::kSuccess);
}
