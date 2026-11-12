zerolog_add_test(zerolog_sys_test sys_test.cc LAYER sys)
target_include_directories(zerolog_sys_test PRIVATE "${ZEROLOG_TESTS_ROOT_DIR}")

zerolog_add_test(zerolog_sys_tsc_clock_test tsc_clock_test.cc LAYER sys)
target_include_directories(zerolog_sys_tsc_clock_test PRIVATE "${ZEROLOG_TESTS_ROOT_DIR}")
target_link_libraries(zerolog_sys_tsc_clock_test PRIVATE Threads::Threads)
