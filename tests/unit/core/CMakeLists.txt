foreach(name IN ITEMS spsc_var_queue doorbell registry_thread_buffer lease_span)
  zerolog_add_test(zerolog_core_${name}_test "${name}_test.cc" LAYER core)
  target_include_directories(zerolog_core_${name}_test PRIVATE "${ZEROLOG_TESTS_ROOT_DIR}")
  target_link_libraries(zerolog_core_${name}_test PRIVATE Threads::Threads)
endforeach()

foreach(name IN ITEMS producer scratch_heap reentrancy)
  zerolog_add_test(zerolog_core_${name}_test "${name}_test.cc" LAYER core)
  target_include_directories(zerolog_core_${name}_test PRIVATE "${ZEROLOG_TESTS_ROOT_DIR}")
endforeach()

zerolog_add_test(zerolog_core_consumer_test consumer_test.cc LAYER core)
target_include_directories(zerolog_core_consumer_test PRIVATE "${ZEROLOG_TESTS_ROOT_DIR}")
target_link_libraries(zerolog_core_consumer_test PRIVATE Threads::Threads)

set_tests_properties(zerolog_core_producer_test zerolog_core_consumer_test PROPERTIES TIMEOUT 30)
