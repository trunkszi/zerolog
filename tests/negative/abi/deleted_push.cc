// expected-error: deleted
#include <zerolog/abi/contract.hpp>
int main() {
  zerolog::InplaceVector<int, 2> v;
  v.push_back(1);
}
