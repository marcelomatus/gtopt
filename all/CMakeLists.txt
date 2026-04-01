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

  cmake -S all -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++
  cmake --build build -j$(nproc)
  cmake --install build          # installs to ~/.local for non-root users

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

``GTOPT_BUILD_WHITE_PAPER``
  Build the white paper PDF from LaTeX sources (requires ``latexmk``
  and a TeX distribution with ``pdflatex`` and ``IEEEtran``; disabled
  by default).

``ENABLE_TEST_COVERAGE``
  Instrument all C++ targets (unit tests, gtopt library, standalone binary)
  with ``-fprofile-arcs -ftest-coverage`` so that ``lcov --capture
  --directory build`` captures both unit-test and e2e-run coverage data.

#]=======================================================================]

cmake_minimum_required(VERSION 3.31.6)

project(BuildAll LANGUAGES CXX)

# ---- Shared cmake module path ----
# Establish cmake_local/ and cmake/ on the module search path early so that
# all include() calls below can use module names where convenient.
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
     "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Build type default ----
# Sets CMAKE_BUILD_TYPE to Debug when the user has not specified one.
# Must come before any add_subdirectory() so the default is visible to all
# sub-projects.
include(BuildTypeDefault)

# ---- Install prefix default ----
# Defaults CMAKE_INSTALL_PREFIX to ~/.local for non-root users.
# Must come before add_subdirectory() so install() rules in sub-projects see
# the correct prefix.
include(DefaultInstallPrefix)

# ---- Component options ----

option(GTOPT_BUILD_STANDALONE  "Build and install the gtopt solver binary"          ON)
option(GTOPT_BUILD_TESTS       "Build the unit + integration test suite"             ON)
option(GTOPT_BUILD_INTEGRATION_TESTS
       "Build and run e2e integration tests (requires GTOPT_BUILD_STANDALONE)"       ON)
option(GTOPT_BUILD_SCRIPTS     "Install the Python conversion scripts"               ON)
option(GTOPT_BUILD_WEBSERVICE  "Install the Next.js web service (requires npm)"      ON)
option(GTOPT_BUILD_GUISERVICE  "Install the Flask GUI service (requires Python 3.10)" ON)
option(GTOPT_BUILD_DOCS        "Build Doxygen documentation (needs internet)"        OFF)
option(GTOPT_BUILD_WHITE_PAPER "Build white paper PDF (requires latexmk + TeX)"     OFF)
option(ENABLE_TEST_COVERAGE    "Enable coverage instrumentation (unit + e2e tests)"  OFF)

# ---- ccache ----
# Must be configured before tools.cmake and before CPMAddPackage(gtopt) so
# that the compiler launcher is in effect for every compilation unit —
# including CMake feature-test compilations during configure.
#
# Ccache.cmake honours any CMAKE_<LANG>_COMPILER_LAUNCHER value already set
# by the caller (e.g. CI's -DCMAKE_CXX_COMPILER_LAUNCHER=ccache) and only
# sets the launchers when they are not yet provided.
#
# Note: cmake/tools.cmake also handles USE_CCACHE, but the option() is scoped
# to the gtopt library subproject (processed later via CPMAddPackage) and is
# therefore not visible here.  Ccache.cmake is the canonical place for the
# all/ super-project build.
include(Ccache)

# ---- Sanitizers / static analysis (optional) ----
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/tools.cmake")

# ---- gtopt library ----
# Build (or locate) the gtopt library exactly once, before any component
# sub-project is processed.  This guarantees a single compiled copy shared by
# standalone, tests, and integration tests, all compiled in the same mode set
# above.
#
# With CPM_USE_LOCAL_PACKAGES=ON (the default) CPM calls find_package(gtopt)
# first; if a previously installed library is found it is reused without
# recompilation.  In-tree sharing (the usual cmake -S all scenario) is
# achieved via the if(NOT TARGET gtopt::gtopt) guard — no installation is
# required for a development build.
option(CPM_USE_LOCAL_PACKAGES "Try find_package before building from source" ON)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake")
if(NOT TARGET gtopt::gtopt)
  CPMAddPackage(NAME gtopt SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

# ---- Testing ----
# enable_testing() must be called in the top-level CMakeLists.txt (or the
# directory that owns the CTestTestfile) for ctest to discover tests registered
# by any sub-project.
enable_testing()
include(TestParallelism)

# ---- Subprojects ----

if(GTOPT_BUILD_STANDALONE)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../standalone" standalone)
endif()

if(GTOPT_BUILD_TESTS)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../test" test)
endif()

if(GTOPT_BUILD_INTEGRATION_TESTS)
  if(NOT GTOPT_BUILD_STANDALONE)
    message(WARNING
      "GTOPT_BUILD_INTEGRATION_TESTS requires GTOPT_BUILD_STANDALONE — "
      "disabling integration tests."
    )
  else()
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../integration_test" integration_test)
  endif()
endif()

if(GTOPT_BUILD_SCRIPTS)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../scripts" scripts)
endif()

if(GTOPT_BUILD_WEBSERVICE)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../webservice" webservice)
endif()

if(GTOPT_BUILD_GUISERVICE)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../guiservice" guiservice)
endif()

if(GTOPT_BUILD_DOCS)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../documentation" documentation)
endif()

if(GTOPT_BUILD_WHITE_PAPER)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../docs/white_paper" white_paper)
endif()

option(GTOPT_BUILD_COVERAGE  "Build coverage analysis sub-project"  OFF)
option(GTOPT_BUILD_PROFILING "Build profiling analysis sub-project" OFF)

if(GTOPT_BUILD_COVERAGE)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../coverage" coverage)
endif()

if(GTOPT_BUILD_PROFILING)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../profiling" profiling)
endif()

# ---- Uninstall target ----
# `cmake --build build --target uninstall` removes every file recorded in
# build/install_manifest.txt by the last `cmake --install` run.
include(UninstallTarget)
