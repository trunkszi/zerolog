add_executable(zerolog_gem5_harness gem5_harness.cc)
# gem5_harness.cc includes format/header_cache.hpp, which sits above encode.
zerolog_configure_internal(zerolog_gem5_harness)
set_target_properties(zerolog_gem5_harness PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
