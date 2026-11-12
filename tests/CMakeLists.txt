find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(ZEROLOG_TESTS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(consteval)
add_subdirectory(unit)
add_subdirectory(negative)
add_subdirectory(codegen)
add_subdirectory(e2e)
