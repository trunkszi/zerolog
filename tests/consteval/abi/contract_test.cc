#include <array>
#include <zerolog/abi/record_layout.hpp>
using namespace zerolog::abi;
static_assert(LevelGateWord{}.with<^^LevelGateSpec::min_level>(Level::kWarn).get<^^LevelGateSpec::min_level>() == Level::kWarn);
static_assert(DoorbellWord{}.with<^^DoorbellSpec::parked>(true).test<^^DoorbellSpec::parked>());
static_assert(ProducerMetaWord{}.with<^^ProducerMetaSpec::free_blocks>(262144).get<^^ProducerMetaSpec::free_blocks>() ==
              262144);
static_assert(SlotEpochWord{}
                  .with<^^SlotEpochSpec::epoch>(0xffffffffU)
                  .wrapping_add_top_field<^^SlotEpochSpec::epoch>(1)
                  .get<^^SlotEpochSpec::epoch>() == 0);
static_assert(ConsumerStateWord{}.with<^^ConsumerStateSpec::active>(true).test<^^ConsumerStateSpec::active>());
static_assert(DrainFlagsWord{}.with<^^DrainFlagsSpec::backpressure>(true).raw() == 1);
static_assert(SinkFlagsWord{}.with<^^SinkFlagsSpec::lifecycle>(LifecycleState::kClosed).get<^^SinkFlagsSpec::lifecycle>() ==
              LifecycleState::kClosed);
static_assert(RecordHeaderWord{}
                  .with<^^RecordHeaderSpec::blocks>(4)
                  .with<^^RecordHeaderSpec::level>(Level::kInfo)
                  .get<^^RecordHeaderSpec::blocks>() == 4);
static_assert(sizeof(StaticRecordInfo) == 64 && offsetof(StaticRecordInfo, column) == 60);
consteval bool Vector() {
  zerolog::InplaceVector<int, 4> v;
  if (!v.try_push_back(1) || !v.try_emplace_back(2)) return false;
  std::array values{3, 4, 5};
  if (v.try_append_range(values) != values.begin() + 2 || !v.full() || v.try_push_back(6)) return false;
  v.erase(v.begin() + 1, v.begin() + 3);
  if (v.size() != 2 || v[1] != 4 || v.try_resize(5)) return false;
  if (!v.try_resize(3, 7) || v.back() != 7 || !v.try_resize(1)) return false;
  zerolog::InplaceVector<int, 4> w;
  w.unchecked_push_back(9);
  w.unchecked_push_back(10);
  v.swap(w);
  if (v.size() != 2 || w.size() != 1 || w[0] != 1 || *v.rbegin() != 10) return false;
  const auto copy = v;
  if (!(copy == v) || (copy <=> v) != 0) return false;
  zerolog::InplaceVector<int, 0> z;
  if (!z.empty() || !z.full() || z.try_emplace_back(1) || z.try_resize(1) || !z.try_resize(0)) return false;
  z.clear();
  z.swap(z);
  return z.erase(z.begin(), z.end()) == nullptr;
}
static_assert(Vector());
struct Immovable {
  Immovable() = delete;
  Immovable(const Immovable&) = delete;
  ~Immovable() noexcept { std::abort(); }
};
static_assert(std::is_trivially_copyable_v<zerolog::InplaceVector<int, 4>>);
static_assert(std::is_empty_v<zerolog::InplaceVector<Immovable, 0>>);
static_assert(std::is_trivially_copyable_v<zerolog::InplaceVector<Immovable, 0>>);
int main() {}

consteval bool AbiVectors() {
  zerolog::InplaceVector<FormatSegment, 8> segments;
  if (!segments.try_emplace_back(FormatSegment{.text = "abc", .text_len = 3})) return false;
  if (segments.front().text_len != 3 || segments.back().precision != 6) return false;
  zerolog::InplaceVector<HeapNode, 64> heap;
  if (!heap.try_emplace_back(HeapNode{42, 3, 4})) return false;
  return heap[0].tsc == 42;
}
static_assert(AbiVectors());
static_assert(std::is_trivially_copyable_v<zerolog::InplaceVector<FormatSegment, 8>>);
static_assert(std::is_trivially_copyable_v<zerolog::InplaceVector<HeapNode, 64>>);
inline constexpr auto kPromotedInfo = std::define_static_array(std::array{StaticRecordInfo{}}).data();
static_assert(kPromotedInfo->level == Level::kInfo);

static_assert(std::is_same_v<zerolog::InplaceVector<int, 4>, uarch::container::freestanding::inplace_vector<int, 4>>);
