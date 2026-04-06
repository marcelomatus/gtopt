cmake_minimum_required(VERSION 3.31.6)

project(gtoptTests LANGUAGES CXX)

# ---- Shared cmake module path ----
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
     "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Build defaults ----
include(BuildTypeDefault)
include(CxxStandard)
include(Ccache)

# ---- Options ----
option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)

set(CPPCHECK_ARGS
    "--check-level=exhaustive" "--suppress=returnDanglingLifetime" "--suppress=internalAstError"
)

# ---- Compiler checks and tools ----
include(CompilerCheck)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/tools.cmake")

# ---- Dependencies ----
option(CPM_USE_LOCAL_PACKAGES "Try find_package first" ON)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake")

if(TEST_INSTALLED_VERSION)
  find_package(gtopt REQUIRED)
elseif(NOT TARGET gtopt::gtopt)
  CPMAddPackage(NAME gtopt SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

CPMAddPackage(
  NAME doctest
  GITHUB_REPOSITORY doctest/doctest
  VERSION 2.5.0
  DOWNLOAD_ONLY YES
)
add_library(doctest::doctest INTERFACE IMPORTED)
target_include_directories(doctest::doctest INTERFACE "${doctest_SOURCE_DIR}")
# Force __COUNTER__ for anonymous names: with unity builds, multiple .cpp files
# are batched into single TUs, so __LINE__ collisions occur when two files have
# TEST_CASE on the same line number.  Doctest 2.5 switched to __LINE__ for
# clang >= 22, but __COUNTER__ is required for unity builds.
# This also works with doctest 2.4 (which already defaults to __COUNTER__).
target_compile_definitions(doctest::doctest INTERFACE "DOCTEST_COUNTER=__COUNTER__")
message(STATUS "Using doctest ${CPM_PACKAGE_doctest_VERSION}")

include(ClangFormat)

# ---- Test executable ----
file(GLOB sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/source/json/[a-zA-Z]*.cpp"
)
list(FILTER sources EXCLUDE REGEX "test_benchmark_")

add_executable(${PROJECT_NAME} ${sources})

target_link_libraries(
  ${PROJECT_NAME}
  doctest::doctest
  gtopt::gtopt
)

# ---- Unity build ----
# Exclude main.cpp from unity batching — it defines
# DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN which emits doctest's implementation
# and must be compiled as a standalone translation unit.
set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/source/main.cpp"
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)
include(UnityBuild)
target_enable_unity_build(${PROJECT_NAME})

# ---- Precompiled headers for tests ----
target_precompile_headers(${PROJECT_NAME} PRIVATE
  <algorithm>
  <chrono>
  <cmath>
  <cstdint>
  <expected>
  <filesystem>
  <format>
  <fstream>
  <functional>
  <map>
  <memory>
  <numeric>
  <optional>
  <ranges>
  <set>
  <span>
  <string>
  <string_view>
  <unordered_map>
  <utility>
  <variant>
  <vector>
  <spdlog/spdlog.h>
)

if(NOT TEST_INSTALLED_VERSION)
  include(CompilerWarnings)
  target_set_warnings(${PROJECT_NAME})
  target_compile_definitions(
    ${PROJECT_NAME}
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:DOCTEST_CONFIG_USE_STD_HEADERS>"
  )
endif()

# ---- Ensure solver plugins are rebuilt before running tests ----
include(GtoptPlugin)
gtopt_require_solver_plugins(${PROJECT_NAME})

# ---- Test registration ----
enable_testing()
include(TestParallelism)
include("${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
doctest_discover_tests(${PROJECT_NAME})

# ---- Coverage ----
include(Coverage)
if(ENABLE_TEST_COVERAGE)
  target_enable_coverage(${PROJECT_NAME})
  # Also instrument the gtopt library itself so that source/*.cpp files are
  # included in the coverage report (not just the headers).
  if(NOT TEST_INSTALLED_VERSION AND TARGET gtopt)
    target_enable_coverage(gtopt PRIVATE)
  endif()
endif()
