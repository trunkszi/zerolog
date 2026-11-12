zerolog_add_test(zerolog_abi_appender_test appender_test.cc LAYER abi)

foreach(target IN ITEMS zerolog_abi_appender_test)
  target_include_directories(${target} PRIVATE "${ZEROLOG_TESTS_ROOT_DIR}")
endforeach()
