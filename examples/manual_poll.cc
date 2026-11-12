#include <cstdio>
#include <zerolog/zerolog.hpp>

int main() {
  zerolog::PrewarmThread();
  zerolog::Info("manual poll ready");
  auto drained = zerolog::Poll(true);
  if (!drained) return static_cast<int>(drained.error());
  std::printf("records=%u bytes=%u\n", drained->records, drained->bytes);
  return static_cast<int>(zerolog::ErrorCode::kSuccess);
}
