foreach(name IN ITEMS abi_sys ring_registry encode_format producer_consumer sink hot_path)
  if(name STREQUAL "hot_path")
    set(source "${PROJECT_SOURCE_DIR}/src/probes/hot_path_probes.cc")
  else()
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${name}_probes.cc")
  endif()
  zerolog_add_codegen_probes(zerolog_${name}_probes "${source}")
  add_test(
    NAME zerolog_${name}_codegen
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/zerolog"
      verify --tier 1
      --object $<TARGET_OBJECTS:zerolog_${name}_probes>
      --gates "${source}"
      --ir $<TARGET_PROPERTY:zerolog_${name}_probes,ZEROLOG_IR_OUTPUTS>)
endforeach()
