cmake_minimum_required(VERSION 3.31.6)

# ---- Project ----

project(
  gtopt
  VERSION 1.0
  LANGUAGES CXX
)

# ---- C++ standard, export compile commands, and cmake policy ----
# Uses cmake_local/CxxStandard.cmake to centralise standard settings.
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_SOURCE_DIR}/cmake_local"
     "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
)
include(CxxStandard)
include(FastLinker)

include(CompilerCheck)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds not allowed.")
endif()

# ---- CPM dependency manager ----

set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/cpm")
set(CPM_USE_LOCAL_PACKAGES ON)
set(CPM_DOWNLOAD_ALL OFF)

set(CPPCHECK_ARGS
    "--check-level=exhaustive" "--suppress=returnDanglingLifetime" "--suppress=internalAstError"
)

include(cmake/CPM.cmake)

# ---- ccache ----
include(Ccache)

# ---- Sanitizers / static analysis (optional) ----
include(cmake/tools.cmake)

include(cmake/ClangFormat.cmake)

# ---- Install git pre-commit hook (auto-formats staged C/C++ files) ----

find_program(GIT_PROGRAM git)
if(GIT_PROGRAM)
  execute_process(
    COMMAND ${GIT_PROGRAM} config core.hooksPath .githooks
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE _hooks_result
    ERROR_QUIET
  )
  if(_hooks_result EQUAL 0)
    message(STATUS "git hooks path set to .githooks (pre-commit auto-format enabled)")
  endif()
endif()

# ---- External libraries ----

CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  VERSION 1.15.0
  OPTIONS "SPDLOG_USE_STD_FORMAT 1" "SPDLOG_INSTALL NO"
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME daw
  GITHUB_REPOSITORY beached/daw_json_link
  VERSION 3.31.0
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME strong_type
  GITHUB_REPOSITORY rollbear/strong_type
  VERSION 15
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
)

# ---- Solver configuration ----
# COIN_ROOT_DIR, CPLEX_ROOT_DIR, COIN_SOLVER, and CMAKE_MODULE_PATH are
# all managed inside cmake_local/Solver.cmake.
include(cmake_local/Solver.cmake)

# ---- External packages ----

find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(Boost REQUIRED COMPONENTS container)
find_package(ZLIB REQUIRED)
find_package(zstd REQUIRED)

# ---- Add source files ----

file(GLOB_RECURSE headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/[a-zA-Z]*.hpp"
)
file(GLOB_RECURSE sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp"
)

# Move gtopt_main.cpp to the front of the source list so that Ninja
# schedules its compilation before the unity batches.  It is the slowest
# file (~209s) and compiles standalone (SKIP_UNITY_BUILD_INCLUSION ON),
# so starting it first minimises the critical-path build time.
set(_priority_src "${CMAKE_CURRENT_SOURCE_DIR}/source/gtopt_main.cpp")
list(REMOVE_ITEM sources "${_priority_src}")
list(PREPEND sources "${_priority_src}")

# ---- Create library ----

add_library(${PROJECT_NAME} ${headers} ${sources})

# PIC is required because the static libgtopt.a is linked into shared
# library solver plugins (libgtopt_solver_*.so).  Without PIC, linkers
# like mold reject the R_X86_64_PC32 relocations.
set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# being a cross-platform target, we enforce standards conformance on MSVC
target_compile_options(
  ${PROJECT_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->"
)

target_include_directories(
  ${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_include_directories(
  ${PROJECT_NAME} SYSTEM PUBLIC ${Boost_INCLUDE_DIRS}
)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC spdlog::spdlog
         daw::daw-json-link
         strong_type::strong_type
         Arrow::arrow_shared
         Parquet::parquet_shared
         Boost::container
         ZLIB::ZLIB
         $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
         ${CMAKE_DL_LIBS}
         m
)

# ---- Mixed-compiler C++ standard library linkage ----
# When the CXX compiler is Clang and the C compiler is a different toolchain
# (e.g. CC=gcc-14 CXX=clang-21), the linker may not receive -lstdc++ on the
# command line, causing "DSO missing from command line" errors for libstdc++
# symbols referenced by the static libgtopt.a.  Explicitly link libstdc++ on
# non-Apple platforms where Clang uses libstdc++ by default.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT APPLE)
  target_link_libraries(${PROJECT_NAME} PUBLIC stdc++)
endif()

# ---- Unity build ----
# Exclude gtopt_main.cpp from unity batching — it pulls in the full JSON
# and LP machinery (json_planning.hpp, planning_lp.hpp, etc.) and takes
# ~209s to compile vs ~30-50s for other files.  Compiling it standalone
# lets it run in parallel with unity batches instead of blocking one.
set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/source/gtopt_main.cpp"
  PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON
)
include(UnityBuild)
target_enable_unity_build(${PROJECT_NAME})

# ---- Precompiled headers ----
include(PrecompiledHeaders)
target_gtopt_pch(${PROJECT_NAME})

# enable compiler warnings
include(cmake_local/CompilerWarnings.cmake)
target_set_warnings(${PROJECT_NAME})

# ---- Solver plugins (dlopen-loaded at runtime) ----
add_subdirectory(plugins)

# ---- Create an installable target ----
# this allows users to install and find the library via `find_package()`. When
# included as a subproject (e.g. from standalone), only the namespaced alias and
# version header are created; install rules are skipped.

string(TOLOWER ${PROJECT_NAME}/version.hpp VERSION_HEADER_LOCATION)

include(cmake_local/PackageTarget.cmake)
package_target(
  NAME
  ${PROJECT_NAME}
  VERSION
  ${PROJECT_VERSION}
  NAMESPACE
  ${PROJECT_NAME}
  BINARY_DIR
  ${PROJECT_BINARY_DIR}
  INCLUDE_DIR
  ${PROJECT_SOURCE_DIR}/include
  INCLUDE_DESTINATION
  include/${PROJECT_NAME}-${PROJECT_VERSION}
  VERSION_HEADER
  "${VERSION_HEADER_LOCATION}"
  COMPATIBILITY
  SameMajorVersion
  DEPENDENCIES
  "spdlog 1.15.0"
  "strong_type 15"
  "daw 3.31.0"
)
