cmake_minimum_required(VERSION 3.31.6)

# ---- Project ----

project(
  gtopt
  VERSION 1.0
  LANGUAGES CXX
)

# ---- Ninja job pools (limit parallelism to avoid OOM) ----
# cmake -j still controls the *default* pool; these pools cap the maximums.
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
  set(NPROC 4)
endif()
# Link jobs are memory-heavy; allow fewer concurrent links than compiles.
math(EXPR LINK_JOBS "(${NPROC} + 1) / 2")  # ≈ nproc/2, at least 1

set_property(GLOBAL PROPERTY JOB_POOLS
  compile=${NPROC}
  link=${LINK_JOBS}
)
set(CMAKE_JOB_POOL_COMPILE compile)
set(CMAKE_JOB_POOL_LINK    link)

# ---- C++ standard, export compile commands, and cmake policy ----
# Uses cmake_local/CxxStandard.cmake to centralise standard settings.
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_SOURCE_DIR}/cmake_local"
     "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
)
include(CxxStandard)
include(BuildTypeDefault)
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

# Build spdlog as a SHARED library from source.  The default static
# libspdlog.a (and the system /usr/local static lib CPM would otherwise reuse)
# is NOT position-independent, so it cannot be linked into the solver-plugin
# MODULE .so's (ld: "relocation R_X86_64_PC32 against 'vtable for
# spdlog::sinks::…'; recompile with -fPIC").  A shared libspdlog.so is PIC and,
# as a single runtime instance shared by the core and every dlopen'd plugin,
# gives ONE global logger/registry — so plugin log lines route through the same
# (level-aware, async) logger the core configured.  We must force a from-source
# build (CPM_USE_LOCAL_PACKAGES OFF for this package only) because the local
# package is the static, non-PIC variant.  SPDLOG_INSTALL YES so the .so is
# installed alongside the binaries that now depend on it at runtime.
set(_gtopt_save_cpm_local "${CPM_USE_LOCAL_PACKAGES}")
set(CPM_USE_LOCAL_PACKAGES OFF)
CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  VERSION 1.15.0
  OPTIONS "SPDLOG_USE_STD_FORMAT 1" "SPDLOG_INSTALL YES" "SPDLOG_BUILD_SHARED ON"
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
)
set(CPM_USE_LOCAL_PACKAGES "${_gtopt_save_cpm_local}")
unset(_gtopt_save_cpm_local)

CPMAddPackage(
  NAME daw
  GITHUB_REPOSITORY beached/daw_json_link
  VERSION 3.31.0
  DOWNLOAD_ONLY YES
)

# Patch daw_json_link: in daw_to_json.h the call
# `json_details::serialization::set_bits` inside `namespace json_details`
# resolves as `json_details::json_details::serialization::set_bits` on
# GCC 14/15 and Clang ≥ 21.  Replace with absolute `::daw::json::…`.
#
# This patches the *shared* CPM cache in place and re-runs on every
# configure, so it MUST be idempotent.  The replacement string itself
# contains the search substring, so guard on the already-patched marker
# first — otherwise a second configure would prepend `::daw::json::`
# again and produce the broken `::daw::json::::daw::json::…`.
set(_daw_to_json "${daw_SOURCE_DIR}/include/daw/json/daw_to_json.h")
if(EXISTS "${_daw_to_json}")
  file(READ "${_daw_to_json}" _daw_src)
  string(FIND "${_daw_src}"
    "::daw::json::json_details::serialization::set_bits" _daw_done)
  if(_daw_done EQUAL -1)
    string(FIND "${_daw_src}" "json_details::serialization::set_bits" _daw_pos)
    if(NOT _daw_pos EQUAL -1)
      string(REPLACE
        "json_details::serialization::set_bits"
        "::daw::json::json_details::serialization::set_bits"
        _daw_src "${_daw_src}")
      file(WRITE "${_daw_to_json}" "${_daw_src}")
      message(STATUS "Patched daw_to_json.h: absolute ::daw::json::json_details::serialization::set_bits")
    endif()
  endif()
endif()

CPMAddPackage(
  NAME daw_header_libraries
  GITHUB_REPOSITORY beached/header_libraries
  GIT_TAG master
  GIT_SHALLOW ON
  DOWNLOAD_ONLY YES
)
CPMAddPackage(
  NAME daw_utf_range
  GITHUB_REPOSITORY beached/utf_range
  GIT_TAG v2.2.6
  GIT_SHALLOW ON
  DOWNLOAD_ONLY YES
)
add_library(daw::daw-json-link INTERFACE IMPORTED)
target_include_directories(daw::daw-json-link SYSTEM INTERFACE
  "${daw_SOURCE_DIR}/include"
  "${daw_header_libraries_SOURCE_DIR}/include"
  "${daw_utf_range_SOURCE_DIR}/include"
)
target_compile_definitions(daw::daw-json-link INTERFACE
  DAW_USE_EXCEPTIONS=true
  DAW_NO_FLATTEN
  DAW_NO_FORCED_INLINE
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

# Hard-require snappy support in the Arrow build that gtopt links to.
# Snappy is the parquet spec default codec — any parquet file produced
# by pyarrow / pandas / plp2gtopt / external pipelines without an
# explicit codec override is snappy-compressed, and an Arrow built
# without snappy fails to read those files at runtime with the cryptic
# "Arrow can't open file …" path-fallback message.  ArrowOptions.cmake
# (shipped by Arrow under <prefix>/lib/cmake/Arrow/) records each
# `ARROW_WITH_<CODEC>` boolean from Arrow's own configure step, so we
# can check it directly here.
if(NOT ARROW_WITH_SNAPPY)
  message(FATAL_ERROR
    "The Arrow library at '${ARROW_INSTALL_PREFIX}' was built WITHOUT "
    "snappy support (ARROW_WITH_SNAPPY=OFF in ArrowOptions.cmake).  "
    "gtopt requires it because snappy is the parquet spec default and "
    "is what pyarrow / plp2gtopt / external pipelines produce by "
    "default.  Rebuild Arrow with `-DARROW_WITH_SNAPPY=ON` after "
    "installing libsnappy-dev (Ubuntu/Debian: "
    "`apt install libsnappy-dev`), then re-install Arrow.")
endif()
find_package(Boost REQUIRED COMPONENTS container)
find_package(ZLIB REQUIRED)
find_package(zstd REQUIRED)

# In-memory compression codecs for low_memory mode AND parquet I/O.
# LZ4 is required (preferred codec for in-memory LP snapshots).
# Snappy is required:
#   * It's the parquet spec default (apache/parquet-format), so any
#     parquet file produced by the wider ecosystem (pyarrow, pandas,
#     plp2gtopt without an explicit `-c <codec>` override on a snappy-
#     enabled build) is snappy-compressed.  gtopt MUST be able to
#     read those files.
#   * Apache Arrow's parquet reader links snappy at build time only
#     when libsnappy-dev is present at Arrow's configure step; the
#     check here ensures the Arrow build that gtopt links against
#     was itself built with snappy support (otherwise reads of any
#     snappy parquet fail with "Arrow can't open file …").
# Install: `apt install libsnappy-dev` (Ubuntu/Debian).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
  pkg_check_modules(SNAPPY REQUIRED IMPORTED_TARGET snappy)
else()
  find_library(LZ4_LIBRARY NAMES lz4)
  find_path(LZ4_INCLUDE_DIR NAMES lz4.h)
  if(LZ4_LIBRARY AND LZ4_INCLUDE_DIR)
    set(LZ4_FOUND TRUE)
  else()
    message(FATAL_ERROR "LZ4 not found. Install liblz4-dev (required for in-memory compression).")
  endif()
  find_library(SNAPPY_LIBRARY NAMES snappy)
  find_path(SNAPPY_INCLUDE_DIR NAMES snappy.h)
  if(SNAPPY_LIBRARY AND SNAPPY_INCLUDE_DIR)
    set(SNAPPY_FOUND TRUE)
  else()
    message(FATAL_ERROR
      "Snappy not found.  Install libsnappy-dev (required: parquet "
      "spec default codec; gtopt must read snappy parquet files "
      "produced by pyarrow / plp2gtopt / external pipelines).")
  endif()
endif()

# ---- Add source files ----

file(GLOB_RECURSE headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/[a-zA-Z]*.hpp"
)
file(GLOB_RECURSE sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp"
)

# ---- Naming-dialects registry: compiled-in fallback + path constants ----
# The runtime registry tries an external file first
# (`$GTOPT_NAMING_DIALECTS_FILE`, then the install location, then the
# in-tree source copy); if all of those fail it falls back to the JSON
# content baked at configure time below.  This guarantees the registry
# always works for tests / standalone runs / library consumers that
# skip the install step.
include(GNUInstallDirs)
set(GTOPT_NAMING_DIALECTS_SOURCE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/share/gtopt/naming_dialects.json")
set(GTOPT_NAMING_DIALECTS_INSTALL_PATH
    "${CMAKE_INSTALL_FULL_DATADIR}/gtopt/naming_dialects.json")
file(READ "${GTOPT_NAMING_DIALECTS_SOURCE_PATH}"
     GTOPT_NAMING_DIALECTS_JSON_CONTENT)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/gtopt/names_builtin.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/gtopt/names_builtin.hpp"
  @ONLY
)
list(APPEND headers
     "${CMAKE_CURRENT_BINARY_DIR}/generated/gtopt/names_builtin.hpp")

# ---- Unit-dialects registry: same wiring, sibling file ----
# Bakes `share/gtopt/unit_dialects.json` into a compiled-in fallback so
# the runtime UnitRegistry (`source/unit_registry.cpp`) always has a
# working dictionary even without an installed copy.
set(GTOPT_UNIT_DIALECTS_SOURCE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/share/gtopt/unit_dialects.json")
set(GTOPT_UNIT_DIALECTS_INSTALL_PATH
    "${CMAKE_INSTALL_FULL_DATADIR}/gtopt/unit_dialects.json")
file(READ "${GTOPT_UNIT_DIALECTS_SOURCE_PATH}"
     GTOPT_UNIT_DIALECTS_JSON_CONTENT)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/gtopt/units_builtin.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/gtopt/units_builtin.hpp"
  @ONLY
)
list(APPEND headers
     "${CMAKE_CURRENT_BINARY_DIR}/generated/gtopt/units_builtin.hpp")

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

# Work around g++-15 tree-DSE bug exposed by the LP build path
# (`source/linear_problem.cpp::flatten` and `source/line_losses.cpp`):
# under `-O3` the SSA-level Dead Store Elimination pass eliminates a
# live store and the IEEE 9-bus line-losses tests SEGFAULT at every
# optimisation level >= -O1.  Bisect localised the trigger to
# `-ftree-dse` only (not the RTL-level `-fdse`).  Same source compiles
# and passes under g++-15 `-Og` and clang++23 `-O3`, so the work-
# around is gcc-specific.  Tree-DSE removal costs ~0–1% on this
# workload — well worth the correctness recovery.
#
# Scope: g++ 15.x ONLY, and only on the optimised build configs
# (Release, RelWithDebInfo, MinSizeRel).  Pinning to the 15.x major
# lets g++-14 builds still exercise tree-DSE (useful for cross-
# version validation), and lets a future g++ 16+ build skip the
# work-around entirely so we discover whether upstream has fixed
# the underlying issue.  Debug / CIFast use `-O0` where
# `-ftree-dse` is a no-op anyway, so they remain unaffected.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  target_compile_options(
    ${PROJECT_NAME}
    PUBLIC
      "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-fno-tree-dse>"
  )
endif()

target_include_directories(
  ${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                         $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
)

# Install the editable naming-dialects dictionary alongside the
# library.  The runtime loader probes
# `${CMAKE_INSTALL_FULL_DATADIR}/gtopt/naming_dialects.json` (baked at
# configure time into names_builtin.hpp) before falling back to the
# compiled-in copy.
install(FILES "${GTOPT_NAMING_DIALECTS_SOURCE_PATH}"
              "${GTOPT_UNIT_DIALECTS_SOURCE_PATH}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/gtopt")

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

# Required compression codec for low_memory mode (in-memory LP snapshots)
if(TARGET PkgConfig::LZ4)
  target_link_libraries(${PROJECT_NAME} PUBLIC PkgConfig::LZ4)
  target_compile_definitions(${PROJECT_NAME} PUBLIC GTOPT_HAS_LZ4=1)
  message(STATUS "LZ4 found — in-memory codec lz4 enabled")
elseif(LZ4_FOUND)
  target_link_libraries(${PROJECT_NAME} PUBLIC ${LZ4_LIBRARY})
  target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC ${LZ4_INCLUDE_DIR})
  target_compile_definitions(${PROJECT_NAME} PUBLIC GTOPT_HAS_LZ4=1)
  message(STATUS "LZ4 found — in-memory codec lz4 enabled")
endif()

# Optional compression codec for low_memory mode
if(TARGET PkgConfig::SNAPPY)
  target_link_libraries(${PROJECT_NAME} PUBLIC PkgConfig::SNAPPY)
  target_compile_definitions(${PROJECT_NAME} PUBLIC GTOPT_HAS_SNAPPY=1)
  message(STATUS "Snappy found — in-memory codec snappy enabled")
elseif(SNAPPY_FOUND)
  target_link_libraries(${PROJECT_NAME} PUBLIC ${SNAPPY_LIBRARY})
  target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC ${SNAPPY_INCLUDE_DIR})
  target_compile_definitions(${PROJECT_NAME} PUBLIC GTOPT_HAS_SNAPPY=1)
  message(STATUS "Snappy found — in-memory codec snappy enabled")
endif()

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
