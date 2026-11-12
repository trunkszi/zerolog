add_test(
  NAME zerolog_gate_engine_test
  COMMAND "${Python3_EXECUTABLE}" "${ZEROLOG_TESTS_ROOT_DIR}/support/gate_audit_test.py")
add_test(
  NAME zerolog_tidy_adapter_test
  COMMAND "${Python3_EXECUTABLE}" "${ZEROLOG_TESTS_ROOT_DIR}/support/tidy_audit_test.py")
add_test(
  NAME zerolog_tier2_audit_test
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tier2_audit_test.py")

zerolog_add_test(zerolog_pipeline_e2e_test pipeline_e2e_test.cc)
target_link_libraries(zerolog_pipeline_e2e_test PRIVATE pthread)

add_test(NAME zerolog_example_standalone_worker COMMAND zerolog_example_standalone_worker)
add_test(NAME zerolog_example_shared_proactor COMMAND zerolog_example_shared_proactor)
add_test(NAME zerolog_example_manual_poll COMMAND zerolog_example_manual_poll)
