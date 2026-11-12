zerolog_add_test(zerolog_sink_rotation_test rotation_test.cc LAYER sink)
zerolog_add_test(zerolog_sink_fsm_test fsm_test.cc LAYER sink)
zerolog_add_test(zerolog_sink_proactor_test proactor_sink_test.cc LAYER sink)
zerolog_add_test(zerolog_sink_futex_fallback_test futex_fallback_test.cc LAYER sink)
zerolog_add_test(zerolog_sink_worker_thread_test worker_thread_test.cc LAYER sink)

foreach(target IN ITEMS zerolog_sink_proactor_test zerolog_sink_futex_fallback_test zerolog_sink_worker_thread_test)
  target_link_libraries(${target} PRIVATE Threads::Threads)
endforeach()

set_tests_properties(
  zerolog_sink_proactor_test
  zerolog_sink_futex_fallback_test
  zerolog_sink_worker_thread_test
  PROPERTIES TIMEOUT 30)
