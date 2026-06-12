#[=======================================================================[.rst:
gtoptCoverage
-------------

Standalone CMake sub-project that builds gtopt + tests with coverage
instrumentation, runs the full test suite, collects lcov data, generates
an HTML report, and writes a Markdown summary to ``reports/coverage/``.

Quick start::

  # Build and run full coverage analysis:
  cmake -S coverage -B build-coverage \
    [-DCOVERAGE_COMPILER=clang] \
    [-DCOVERAGE_OUTPUT_DIR=/path/to/reports/coverage] \
    [-DCOVERAGE_CMAKE_PREFIX_PATH="$(conda info --base)"]
  cmake --build build-coverage -j$(nproc)

  # Check tool availability:
  cmake --build build-coverage --target check-deps

  # Individual steps:
  cmake --build build-coverage --target coverage-configure
  cmake --build build-coverage --target coverage-build
  cmake --build build-coverage --target coverage-run
  cmake --build build-coverage --target coverage-collect
  cmake --build build-coverage --target coverage-report

Prerequisites
^^^^^^^^^^^^^

Install lcov (required)::

  sudo apt-get install -y lcov

Options
^^^^^^^

``COVERAGE_COMPILER``
  C compiler to use for the instrumented build.  ``"clang"`` (default) or
  ``"gcc"``.  The corresponding C++ compiler is derived automatically
  (``clang++`` / ``g++-14``).

``COVERAGE_OUTPUT_DIR``
  Directory where all output files are written.  Defaults to
  ``<repo_root>/reports/coverage``.  Created automatically if absent.

``COVERAGE_PYTHON``
  ``ON`` (default) — also run ``pytest --cov`` on the scripts/ package and
  generate a Python coverage HTML report alongside the C++ report.

``COVERAGE_CMAKE_PREFIX_PATH``
  Extra ``CMAKE_PREFIX_PATH`` forwarded to the gtopt configure step.
  Required when Arrow/Parquet are installed via conda::

    -DCOVERAGE_CMAKE_PREFIX_PATH="$(conda info --base)"

``COVERAGE_EXTRA_CMAKE_ARGS``
  Semicolon-separated list of additional cmake arguments forwarded to the
  ``coverage-configure`` step.  Example::

    -DCOVERAGE_EXTRA_CMAKE_ARGS="-DGTOPT_SOME_OPTION=ON"

Targets
^^^^^^^

``coverage`` (ALL)
  Runs the full analysis pipeline in order.

``coverage-configure``
  (Re)configures the gtopt build with ``ENABLE_TEST_COVERAGE=ON``.

``coverage-build``
  Builds gtopt + tests in the coverage build directory.

``coverage-run``
  Runs ctest with ``--output-on-failure``.  Test failures do NOT block
  coverage collection — gcda data is still captured.

``coverage-collect``
  Runs lcov to capture ``.gcda`` data, filters out external code, and
  generates the HTML report via genhtml.  Uses ``-j`` for parallel
  processing.

``coverage-report``
  Generates ``coverage-summary.md`` and prints a brief summary.

``coverage-python``
  Runs ``pytest --cov`` on scripts/ (only when ``COVERAGE_PYTHON=ON`` and
  the required tools are found).

``check-deps``
  Prints the status of all required and optional tools.

#]=======================================================================]

cmake_minimum_required(VERSION 3.31.6)

project(gtoptCoverage LANGUAGES NONE)

# ---- Module path --------------------------------------------------------
list(
  APPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Options ------------------------------------------------------------
set(COVERAGE_COMPILER
    "clang"
    CACHE STRING "C compiler for the instrumented build (clang or gcc)"
)
set_property(CACHE COVERAGE_COMPILER PROPERTY STRINGS clang gcc)

get_filename_component(
  _default_output_dir "${CMAKE_CURRENT_LIST_DIR}/../reports/coverage" ABSOLUTE
)
set(COVERAGE_OUTPUT_DIR
    "${_default_output_dir}"
    CACHE PATH "Directory where coverage reports are written"
)

option(COVERAGE_PYTHON "Run Python pytest coverage in addition to C++" ON)

option(COVERAGE_INTEGRATION_TESTS
       "Include integration tests (e2e cases) in the coverage run" ON
)

set(COVERAGE_CMAKE_PREFIX_PATH
    ""
    CACHE PATH
    "Extra CMAKE_PREFIX_PATH for gtopt configure (e.g. conda base for Arrow)"
)

set(COVERAGE_EXTRA_CMAKE_ARGS
    ""
    CACHE STRING "Semicolon-separated extra cmake args for coverage-configure"
)

# ---- Derived settings ---------------------------------------------------
if(COVERAGE_COMPILER STREQUAL "gcc")
  find_program(_gxx_exec NAMES g++-15 g++-14 g++)
  if(_gxx_exec)
    set(_coverage_cxx "${_gxx_exec}")
  else()
    set(_coverage_cxx "g++")
  endif()
  find_program(_gcov_exec NAMES gcov-15 gcov-14 gcov)
  if(_gcov_exec)
    set(_gcov_tool "${_gcov_exec}")
  else()
    set(_gcov_tool "gcov")
    message(STATUS "gcov-15/gcov-14 not found; falling back to 'gcov'")
  endif()
elseif(COVERAGE_COMPILER STREQUAL "clang")
  set(_coverage_cxx "clang++")
  find_program(LLVM_COV_EXECUTABLE NAMES llvm-cov llvm-cov-21 llvm-cov-20 llvm-cov-19)
  if(LLVM_COV_EXECUTABLE)
    # Write the gcov wrapper script needed by lcov
    set(_gcov_wrapper "${CMAKE_BINARY_DIR}/llvm-gcov.sh")
    file(
      WRITE "${_gcov_wrapper}"
      "#!/bin/sh\nexec \"${LLVM_COV_EXECUTABLE}\" gcov \"$@\"\n"
    )
    execute_process(COMMAND chmod +x "${_gcov_wrapper}")
    set(_gcov_tool "${_gcov_wrapper}")
  else()
    # Fall back: assume 'llvm-cov' is on PATH
    set(_gcov_wrapper "${CMAKE_BINARY_DIR}/llvm-gcov.sh")
    file(WRITE "${_gcov_wrapper}" "#!/bin/sh\nexec llvm-cov gcov \"$@\"\n")
    execute_process(COMMAND chmod +x "${_gcov_wrapper}")
    set(_gcov_tool "${_gcov_wrapper}")
    message(STATUS "llvm-cov not found by name; wrapper uses PATH lookup")
  endif()
else()
  message(FATAL_ERROR "COVERAGE_COMPILER must be 'clang' or 'gcc' (got '${COVERAGE_COMPILER}')")
endif()

# ---- Find required tools ------------------------------------------------
include(FindLcov)
find_package(Lcov REQUIRED)

find_program(PYTHON3_EXECUTABLE NAMES python3 python)
find_program(PYTEST_EXECUTABLE NAMES pytest)

# ---- Processor count for parallelism ------------------------------------
include(ProcessorCount)
ProcessorCount(_nproc)
if(NOT _nproc OR _nproc EQUAL 0)
  set(_nproc 4)
endif()

# ---- Paths & directories ------------------------------------------------
set(_repo_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_all_dir "${_repo_root}/all")
set(_coverage_build_dir "${CMAKE_BINARY_DIR}/gtopt-coverage-build")
set(_coverage_raw_info "${COVERAGE_OUTPUT_DIR}/coverage-raw.info")
set(_coverage_info "${COVERAGE_OUTPUT_DIR}/coverage.info")
set(_coverage_html_dir "${COVERAGE_OUTPUT_DIR}/html")

# Ensure output directories exist
file(MAKE_DIRECTORY "${COVERAGE_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${_coverage_html_dir}")

# ---- Print status -------------------------------------------------------
message(STATUS "=== gtopt Coverage Configuration ===")
message(STATUS "  Compiler:     ${COVERAGE_COMPILER} / ${_coverage_cxx}")
message(STATUS "  gcov tool:    ${_gcov_tool}")
message(STATUS "  lcov:         ${LCOV_EXECUTABLE}")
message(STATUS "  genhtml:      ${GENHTML_EXECUTABLE}")
message(STATUS "  python3:      ${PYTHON3_EXECUTABLE}")
message(STATUS "  pytest:       ${PYTEST_EXECUTABLE}")
message(STATUS "  Integration:  ${COVERAGE_INTEGRATION_TESTS}")
message(STATUS "  Parallelism:  ${_nproc} jobs")
message(STATUS "  Output dir:   ${COVERAGE_OUTPUT_DIR}")
message(STATUS "  Build dir:    ${_coverage_build_dir}")
if(NOT PYTHON3_EXECUTABLE OR NOT PYTEST_EXECUTABLE)
  message(STATUS "  Python cov:   disabled (python3/pytest not found)")
  message(STATUS "    Install:    pip install pytest pytest-cov")
endif()

# ---- Build the cmake argument list for configure ------------------------
set(_configure_args
  -S "${_all_dir}"
  -B "${_coverage_build_dir}"
  -DENABLE_TEST_COVERAGE=ON
  -DCMAKE_BUILD_TYPE=Debug
  "-DCMAKE_C_COMPILER=${COVERAGE_COMPILER}"
  "-DCMAKE_CXX_COMPILER=${_coverage_cxx}"
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
)
if(COVERAGE_INTEGRATION_TESTS)
  list(APPEND _configure_args
    -DGTOPT_BUILD_INTEGRATION_TESTS=ON
    -DGTOPT_BUILD_STANDALONE=ON
  )
endif()
if(COVERAGE_CMAKE_PREFIX_PATH)
  list(APPEND _configure_args "-DCMAKE_PREFIX_PATH=${COVERAGE_CMAKE_PREFIX_PATH}")
endif()
if(COVERAGE_EXTRA_CMAKE_ARGS)
  # cmake CACHE STRING stores semicolons as list separators
  separate_arguments(_extra_args NATIVE_COMMAND "${COVERAGE_EXTRA_CMAKE_ARGS}")
  list(APPEND _configure_args ${_extra_args})
endif()

# ---- Helper: write a shell script for coverage-run ----------------------
# We need ctest to NOT fail the pipeline when tests fail, because coverage
# data (.gcda) is still produced by the tests that did run.  CMake custom
# targets treat any non-zero exit code as a build failure, so we wrap ctest
# in a shell script that always returns 0.
set(_ctest_wrapper "${CMAKE_BINARY_DIR}/run-ctest.sh")
file(
  WRITE "${_ctest_wrapper}"
  "#!/bin/sh\n"
  "\"${CMAKE_CTEST_COMMAND}\" "
  "--test-dir \"${_coverage_build_dir}\" "
  "--output-on-failure "
  "-j ${_nproc} "
  "-E 'scripts-(check-format|ruff|format)'\n"
  "exit 0\n"
)
execute_process(COMMAND chmod +x "${_ctest_wrapper}")

# ---- Target: check-deps -------------------------------------------------
add_custom_target(
  check-deps
  COMMAND ${CMAKE_COMMAND} -E echo "=== Coverage tool status ==="
  COMMAND ${CMAKE_COMMAND} -E echo "  lcov:      ${LCOV_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  genhtml:   ${GENHTML_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  gcov tool: ${_gcov_tool}"
  COMMAND ${CMAKE_COMMAND} -E echo "  python3:   ${PYTHON3_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  pytest:    ${PYTEST_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Install missing tools:"
  COMMAND ${CMAKE_COMMAND} -E echo "  sudo apt-get install -y lcov"
  COMMAND ${CMAKE_COMMAND} -E echo "  pip install pytest pytest-cov"
  COMMENT "Checking coverage tool dependencies"
)

# ---- Target: coverage-configure -----------------------------------------
add_custom_target(
  coverage-configure
  COMMAND ${CMAKE_COMMAND} ${_configure_args}
  COMMENT "Configuring gtopt with coverage instrumentation..."
  USES_TERMINAL
  VERBATIM
)

# ---- Target: coverage-build ---------------------------------------------
add_custom_target(
  coverage-build
  COMMAND ${CMAKE_COMMAND} --build "${_coverage_build_dir}" -j ${_nproc}
  COMMENT "Building gtopt + tests with coverage flags..."
  USES_TERMINAL
  VERBATIM
)
add_dependencies(coverage-build coverage-configure)

# ---- Target: coverage-run -----------------------------------------------
add_custom_target(
  coverage-run
  COMMAND "${_ctest_wrapper}"
  COMMENT "Running tests with coverage instrumentation (excluding format/ruff)..."
  USES_TERMINAL
)
add_dependencies(coverage-run coverage-build)

# ---- Target: coverage-collect -------------------------------------------
# lcov 2.x supports -j for parallel processing.
# ignore-error options handle common discrepancies between clang's
# .gcno/.gcda output and lcov's expectations.
add_custom_target(
  coverage-collect
  # Step 1: Capture raw coverage data from build tree (parallel)
  COMMAND
    ${LCOV_EXECUTABLE}
    --capture
    --directory "${_coverage_build_dir}"
    --output-file "${_coverage_raw_info}"
    --gcov-tool "${_gcov_tool}"
    -j ${_nproc}
    --ignore-errors mismatch,mismatch
    --ignore-errors unexecuted,unexecuted
    --ignore-errors negative,negative
    --ignore-errors inconsistent,inconsistent
    --ignore-errors format,format
    --rc geninfo_unexecuted_blocks=1
    --rc branch_coverage=1
  # Step 2: Filter out external/test/third-party files (parallel)
  COMMAND
    ${LCOV_EXECUTABLE}
    --remove "${_coverage_raw_info}"
    "/usr/*"
    "*/cpm_modules/*"
    "*/_deps/*"
    "*/test/*"
    "*/daw/*"
    "*/strong_type/*"
    "*/spdlog/*"
    --output-file "${_coverage_info}"
    --gcov-tool "${_gcov_tool}"
    -j ${_nproc}
    --ignore-errors unused,unused,inconsistent,inconsistent
    --rc branch_coverage=1
  # Step 3: Generate HTML report (parallel)
  COMMAND
    ${GENHTML_EXECUTABLE}
    "${_coverage_info}"
    --output-directory "${_coverage_html_dir}"
    --title "gtopt C++ Coverage"
    --legend
    --show-details
    -j ${_nproc}
    --rc branch_coverage=1
    --ignore-errors inconsistent,inconsistent
    --ignore-errors category,category
  COMMENT "Collecting coverage data and generating HTML report (${_nproc} jobs)..."
  USES_TERMINAL
  VERBATIM
)
add_dependencies(coverage-collect coverage-run)

# ---- Target: coverage-report --------------------------------------------
add_custom_target(
  coverage-report
  COMMAND
    ${CMAKE_COMMAND}
    "-DLCOV_EXECUTABLE=${LCOV_EXECUTABLE}"
    "-DLCOV_INFO_FILE=${_coverage_info}"
    "-DOUTPUT_MD=${COVERAGE_OUTPUT_DIR}/coverage-summary.md"
    "-DGCOV_TOOL=${_gcov_tool}"
    "-DREPO_ROOT=${_repo_root}"
    -P "${CMAKE_CURRENT_LIST_DIR}/GenerateCoverageReport.cmake"
  COMMENT "Generating Markdown coverage report..."
  USES_TERMINAL
  VERBATIM
)
add_dependencies(coverage-report coverage-collect)

# ---- Target: coverage-python --------------------------------------------
if(COVERAGE_PYTHON AND PYTHON3_EXECUTABLE AND PYTEST_EXECUTABLE)
  file(MAKE_DIRECTORY "${COVERAGE_OUTPUT_DIR}/python")
  add_custom_target(
    coverage-python
    COMMAND
      ${PYTHON3_EXECUTABLE} -m pytest
      --cov=cvs2parquet
      --cov=igtopt
      --cov=plp2gtopt
      --cov=pp2gtopt
      --cov=ts2gtopt
      --cov=gtopt_compare
      --cov=gtopt_diagram
      --cov-report=term-missing
      "--cov-report=html:${COVERAGE_OUTPUT_DIR}/python"
      "--cov-report=xml:${COVERAGE_OUTPUT_DIR}/python-coverage.xml"
      -q
    WORKING_DIRECTORY "${_repo_root}/scripts"
    COMMENT "Running Python coverage analysis (scripts/)..."
    USES_TERMINAL
    VERBATIM
  )
elseif(COVERAGE_PYTHON)
  add_custom_target(
    coverage-python
    COMMAND
      ${CMAKE_COMMAND} -E echo
      "Skipping Python coverage: python3=${PYTHON3_EXECUTABLE} pytest=${PYTEST_EXECUTABLE}"
    COMMENT "Python coverage skipped (missing tools)"
  )
endif()

# ---- Target: coverage (ALL) ---------------------------------------------
# The top-level ALL target drives the complete pipeline.
add_custom_target(coverage ALL)
add_dependencies(coverage coverage-report)
if(TARGET coverage-python)
  add_dependencies(coverage coverage-python)
endif()

# ---- Optional: git-commit target ----------------------------------------
include(GitReports)
add_git_commit_reports_target(
  git-commit-coverage-reports
  "${_repo_root}/reports"
)
