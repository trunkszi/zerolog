include_guard(GLOBAL)

function(zerolog_compiler_options output)
  set(options -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Wconversion
    -Wsign-conversion -Wswitch -Werror -ffunction-sections -fdata-sections)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC diagnoses named non-template expansion variables as shadowing their own generated declarations.
    list(APPEND options -Wno-shadow)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND options -Wshadow)
  else()
    message(FATAL_ERROR "zerolog requires a verified GCC or Clang C++26 compiler")
  endif()
  set(${output} "${options}" PARENT_SCOPE)
endfunction()

function(zerolog_enable_warnings target)
  zerolog_compiler_options(options)
  target_compile_options(${target} PRIVATE ${options})
  # Match glibc's 8 MiB default via musl PT_GNU_STACK; proactor_capacity_test explicitly exercises a 128 KiB pthread stack.
  target_link_options(${target} PRIVATE -Wl,-z,stack-size=8388608)
endfunction()
