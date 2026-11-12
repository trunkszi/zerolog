include_guard(GLOBAL)
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
include("${CMAKE_CURRENT_LIST_DIR}/ZerologCompilerOptions.cmake")

function(zerolog_detect_reflection)
  set(CMAKE_CXX_STANDARD 26)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(reflection_flags -freflection)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(reflection_flags -freflection-latest -stdlib=libc++)
  else()
    message(FATAL_ERROR "zerolog requires GCC reflection or clang-p2996 with libc++")
  endif()
  zerolog_compiler_options(warnings)
  set(source [=[
#include <meta>
#include <string_view>
#include <type_traits>
struct Note { int value; };
struct [[=Note{7}]] Record { int first; int second; };
using Spliced = [:^^Record:];
static_assert(std::is_same_v<Record, Spliced>);
constexpr auto kNotes = std::define_static_array(std::meta::annotations_of(^^Record));
static_assert(std::meta::extract<Note>(std::meta::constant_of(kNotes[0])).value == 7);
constexpr auto kMembers = std::define_static_array(
    std::meta::nonstatic_data_members_of(^^Record, std::meta::access_context::current()));
consteval int Count() {
  int count = 0;
  template for (constexpr auto kItem : kMembers) {
    count += std::meta::is_nonstatic_data_member(kItem) ? 1 : 0;
  }
  return count;
}
template<class T> consteval int CountOf() {
  int count = 0;
  template for (constexpr auto kMember : std::define_static_array(
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))) {
    count += std::meta::is_nonstatic_data_member(kMember) ? 1 : 0;
  }
  return count;
}
template<class... Types> using First = Types...[0];
struct Restricted { void Invoke() = delete("unavailable operation"); };
static_assert(Count() == 2 && CountOf<Record>() == 2);
static_assert(std::is_same_v<First<int, double>, int>);
static_assert(std::define_static_string("ok")[0] == 'o');
static_assert(true, std::string_view("ok"));
int main() { return 0; }
]=])
  string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --version
    OUTPUT_VARIABLE compiler_identity RESULT_VARIABLE version_status)
  if(NOT version_status EQUAL 0)
    message(FATAL_ERROR "Unable to inspect the C++ compiler")
  endif()
  string(SHA256 fingerprint
    "${CMAKE_CXX_COMPILER}|${compiler_identity}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_${build_type}}|${CMAKE_SYSROOT}|${reflection_flags}|${warnings}|${source}")
  set(probe "ZEROLOG_HAS_REQUIRED_REFLECTION_${fingerprint}")
  cmake_push_check_state(RESET)
  string(JOIN " " CMAKE_REQUIRED_FLAGS ${reflection_flags} ${warnings})
  set(CMAKE_REQUIRED_LINK_OPTIONS ${reflection_flags})
  check_cxx_source_compiles("${source}" ${probe})
  cmake_pop_check_state()
  if(NOT ${probe})
    message(FATAL_ERROR "zerolog requires working C++26 reflection, structural annotations, expansion statements, pack indexing, dynamic diagnostics and static promotion under strict warnings; see CMakeConfigureLog.yaml")
  endif()
  set(ZEROLOG_REFLECTION_OPTIONS "${reflection_flags}" PARENT_SCOPE)
endfunction()
