#[=======================================================================[.rst:
BuildAll
--------

Unified CMake entry point that configures and installs every gtopt component
in a single pass.  Use this configuration for full builds, CI pipelines, and
when you want a single ``cmake --install`` to deploy the solver binary,
Python conversion scripts, web service, and GUI service together.

For focused single-component builds use the individual sub-project
``CMakeLists.txt`` files in ``standalone/``, ``scripts/``, ``webservice/``,
``guiservice/``, ``test/``, or ``documentation/``.

Quick start::

  cmake -S all -B build-all \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++
  cmake --build build-all -j$(nproc)
  cmake --install build-all          # installs to ~/.local for non-root users

Component options (all ON by default except docs)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``GTOPT_BUILD_STANDALONE``
  Build the gtopt solver binary (requires C++26 compiler, COIN-OR, Arrow).

``GTOPT_BUILD_TESTS``
  Build and register the unit test suite (requires doctest).

``GTOPT_BUILD_INTEGRATION_TESTS``
  Build and run end-to-end integration tests (requires GTOPT_BUILD_STANDALONE).
  Reuses the standalone binary — no recompilation.

``GTOPT_BUILD_SCRIPTS``
  Install the Python conversion scripts (requires Python ≥ 3.8 and pip).

``GTOPT_BUILD_WEBSERVICE``
  Install the Next.js web service (requires Node.js / npm).
  Automatically disabled with a warning when npm is not found.

``GTOPT_BUILD_GUISERVICE``
  Install the Flask GUI service (requires Python ≥ 3.10).
  Automatically disabled with a warning when Python ≥ 3.10 is not found.

``GTOPT_BUILD_DOCS``
  Build Doxygen HTML documentation (requires internet access to fetch
  the m.css theme; disabled by default).

``ENABLE_TEST_COVERAGE``
  Instrument all C++ targets (unit tests, gtopt library, standalone binary)
  with ``-fprofile-arcs -ftest-coverage`` so that ``lcov --capture
  --directory build`` captures both unit-test and e2e-run coverage data.

#]=======================================================================]

cmake_minimum_required(VERSION 3.31.6)

project(BuildAll LANGUAGES CXX)

# ---- Build type ----
# Set a default before any subdirectory is processed so that every C++
# component (library, standalone binary, tests) is compiled in the same mode.
# Passing -DCMAKE_BUILD_TYPE=Release on the command line overrides this default.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE
  )
  set_property(
    CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel
  )
  message(STATUS "Build type not set — defaulting to Debug")
endif()

# Default to ~/.local for non-root users (XDG Base Directory).
# Must be included before add_subdirectory() so the prefix is established
# before any install() rules are processed.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake_local/DefaultInstallPrefix.cmake")

# ---- Component options ----

option(GTOPT_BUILD_STANDALONE  "Build and install the gtopt solver binary"          ON)
option(GTOPT_BUILD_TESTS       "Build the unit + integration test suite"             ON)
option(GTOPT_BUILD_INTEGRATION_TESTS "Build and run e2e integration tests (requires GTOPT_BUILD_STANDALONE)" ON)
option(GTOPT_BUILD_SCRIPTS     "Install the Python conversion scripts"               ON)
option(GTOPT_BUILD_WEBSERVICE  "Install the Next.js web service (requires npm)"      ON)
option(GTOPT_BUILD_GUISERVICE  "Install the Flask GUI service (requires Python 3.10)" ON)
option(GTOPT_BUILD_DOCS        "Build Doxygen documentation (needs internet)"        OFF)
option(ENABLE_TEST_COVERAGE    "Enable coverage instrumentation (unit + e2e tests)"  OFF)

# ---- Tools ----

include(../cmake/tools.cmake)

# ---- gtopt library ----
# Build (or locate) the gtopt library exactly once, before any component
# subproject is processed.  This guarantees:
#   • a single compiled copy shared by standalone, tests, and integration tests;
#   • all C++ targets are compiled with the same build type set above.
#
# With CPM_USE_LOCAL_PACKAGES=ON (the default), CPM calls find_package(gtopt)
# first.  If a previously installed library is found, it is reused without
# recompilation.  Otherwise the library is built from source.
#
# In-tree sharing (the usual cmake -S all scenario) is achieved via the
# if(NOT TARGET gtopt::gtopt) guard in each component — no installation is
# required.  Set -DGTOPT_INSTALL_LIBRARY=ON explicitly when you also want
# cmake --install to deploy the library binary and CMake package files.
option(CPM_USE_LOCAL_PACKAGES "Try find_package before building from source" ON)
include(../cmake/CPM.cmake)
if(NOT TARGET gtopt::gtopt)
  CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

# Needed to generate the test target even when only some subprojects define tests.
enable_testing()

include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
  set(NPROC 1)
endif()
list(APPEND CMAKE_CTEST_ARGUMENTS "--parallel" "${NPROC}")

# ---- Subprojects ----

if(GTOPT_BUILD_STANDALONE)
  add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../standalone ${CMAKE_BINARY_DIR}/standalone
  )
endif()

if(GTOPT_BUILD_INTEGRATION_TESTS)
  if(NOT GTOPT_BUILD_STANDALONE)
    message(WARNING
      "GTOPT_BUILD_INTEGRATION_TESTS requires GTOPT_BUILD_STANDALONE. "
      "Disabling integration tests.")
  else()
    add_subdirectory(
      ${CMAKE_CURRENT_LIST_DIR}/../integration_test
      ${CMAKE_BINARY_DIR}/integration_test
    )
  endif()
endif()

if(GTOPT_BUILD_TESTS)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../test ${CMAKE_BINARY_DIR}/test)
endif()

if(GTOPT_BUILD_SCRIPTS)
  add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../scripts ${CMAKE_BINARY_DIR}/scripts
  )
endif()

if(GTOPT_BUILD_WEBSERVICE)
  add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../webservice ${CMAKE_BINARY_DIR}/webservice
  )
endif()

if(GTOPT_BUILD_GUISERVICE)
  add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../guiservice ${CMAKE_BINARY_DIR}/guiservice
  )
endif()

if(GTOPT_BUILD_DOCS)
  add_subdirectory(
    ${CMAKE_CURRENT_LIST_DIR}/../documentation ${CMAKE_BINARY_DIR}/documentation
  )
endif()

