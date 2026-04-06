#[=======================================================================[.rst:
Scripts
-------

CMake configuration for installing and testing the gtopt Python scripts:
gtopt_compare, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt, and ts2gtopt.

The scripts/ directory is a self-contained Python package with its own
``pyproject.toml``, ``requirements.txt``, and ``requirements-dev.txt``, so
the usual pip workflows all work without touching the repository root:

  pip install ./scripts                 # production install (from repo root)
  pip install -e ./scripts              # editable install (from repo root)
  pip install -r scripts/requirements.txt            # deps only
  pip install -r scripts/requirements-dev.txt        # deps + dev/test tools

Build targets
^^^^^^^^^^^^^

``scripts-pip-requirements``
  Install only the runtime dependencies from ``scripts/requirements.txt``
  into the active Python environment.

``scripts-install``
  Editable install of the package together with all dev/test dependencies
  (equivalent to ``pip install -e scripts/[dev]``).

``scripts-format``
  Auto-format all Python source files with ruff format (modifies files in-place).

``scripts-check-format``
  Check formatting with ruff format without modifying files; fails if any file
  would be reformatted.

``scripts-lint``
  Run pylint over gtopt_compare, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt, and ts2gtopt;
  exit non-zero on any error/warning.  Registered as a CTest test only when
  ``-DENABLE_SCRIPTS_STATIC_ANALYSIS=ON`` is set (default: OFF).

``scripts-ruff``
  Run ruff check over gtopt_compare, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt, and ts2gtopt;
  exit non-zero on any lint violation.

``scripts-mypy``
  Run mypy static type checking; exit non-zero on any type error.  Registered as
  a CTest test only when ``-DENABLE_SCRIPTS_STATIC_ANALYSIS=ON`` is set
  (default: OFF).

``scripts-test``
  Run the full unit-test suite (gtopt_compare, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt,
  ts2gtopt) with pytest.

``scripts-test-integration``
  Run the integration tests (marked ``integration``) from plp2gtopt, igtopt,
  and ts2gtopt in one shot via pytest (serial; CTest handles parallelism when
  using the individual per-case CTest entries).  For fine-grained, per-case
  control use the individual CTest tests (see below).

CTest / ``cmake --build <dir> --target test``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Running ``cmake --build <dir> --target test`` (or ``ctest`` inside the build
directory) invokes all registered tests.  A ``scripts-install-deps`` fixture
runs first and performs the equivalent of ``pip install -e scripts/[dev]`` so
pytest, ruff, pylint, and mypy are available even in a fresh environment.  All
other tests declare ``FIXTURES_REQUIRED scripts-deps`` so they will be skipped
if the setup fixture fails.

Unit tests are registered as **individual per-package CTest entries** — one per
Python package — so they can run in parallel and be reported separately.  Every
unit test carries the ``unit`` label:

* ``scripts-<package>-unit`` — one test per package directory
  (gtopt_compare, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt,
  ts2gtopt).

Run all unit tests in parallel with::

  ctest -L unit --parallel

Integration tests are registered as **individual per-case CTest entries** — one
per case group — so they can also run in parallel.  Every integration test
carries the ``integration`` label:

* ``scripts-plp2gtopt-<case>`` — one test per plp2gtopt case
  (dat_ex, min_1bus, min_2bus, min_bess, min_battery, min_ess, min_hydro,
  min_mance, min_manli, min_reservoir).
* ``scripts-igtopt-<case>`` — one test per igtopt xlsx case
  (igtopt_ieee57b, igtopt_bat4b24).
* ``scripts-ts2gtopt-<class>`` — one test per ts2gtopt integration class
  (demand_projection, solar_projection, generator_pmax, write_files).

Run all integration tests in parallel with::

  ctest -L integration --parallel

Run a single test::

  ctest -R scripts-plp2gtopt-min_1bus
  ctest -R scripts-gtopt_compare-unit

``scripts-coverage``
  Run unit + integration tests and generate an HTML coverage report under
  ``<build>/scripts-coverage-report/``.

Installation
^^^^^^^^^^^^

To install the scripts alongside the gtopt binary::

  cmake -S scripts -B build-scripts
  cmake --install build-scripts

The install step always uses the Python found at **configure time**
(``${PYTHON_EXECUTABLE} -m pip``).  The absolute path is baked into
``cmake_install.cmake`` so it is immune to the PATH-stripping that
``sudo`` applies by default, making both ``cmake --install`` (as a
regular user) and ``sudo cmake --install`` (system-wide) work
correctly with the same Python.

To avoid a known setuptools issue where a root-owned ``gtopt_scripts.egg-info``
directory left by a previous ``sudo`` install blocks subsequent non-root
rebuilds, the install step copies the source tree to a temporary staging
directory (inside the cmake binary directory, excluding ``*.egg-info``) before
invoking pip, and removes the staging directory afterwards.

#]=======================================================================]

cmake_minimum_required(VERSION 3.31.6)

project(gtoptScripts LANGUAGES NONE)

# ---- Shared cmake module path ----
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
     "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# Default to ~/.local for non-root users (XDG Base Directory)
include(DefaultInstallPrefix)

# ---- Find Python ----

find_program(PYTHON_EXECUTABLE NAMES python3 python REQUIRED)

execute_process(
  COMMAND ${PYTHON_EXECUTABLE} -c
    "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)"
  RESULT_VARIABLE _py_ver_ok
  OUTPUT_QUIET ERROR_QUIET
)
if(NOT _py_ver_ok EQUAL 0)
  message(FATAL_ERROR "Python 3.8 or later is required. Found: ${PYTHON_EXECUTABLE}")
endif()

# scripts/ is the self-contained package root (has its own pyproject.toml)
set(SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- Module lists (avoids repeating the same paths in every target) ----
#
# _SCRIPT_MODULES   – directory-based package names (no path prefix, no .py).
#                     Used verbatim for pylint and mypy invocations.
# _SCRIPT_SOURCES   – full paths for ruff format/check (directories + the
#                     package directories).
# _SCRIPT_TEST_DIRS – full paths to each package's tests/ subdirectory,
#                     used for pytest unit-test and coverage invocations.
# _SCRIPT_COV_ARGS  – --cov=<path> arguments for the pytest-cov plugin.

set(_SCRIPT_MODULES
  gtopt_compare
  cvs2parquet
  gtopt_diagram
  gtopt_field_extractor
  gtopt_check_lp
  gtopt_check_json
  gtopt_check_output
  gtopt_check_solvers
  gtopt_compress_lp
  gtopt_config
  gtopt2pp
  igtopt
  plp2gtopt
  pp2gtopt
  run_gtopt
  gtopt_monitor
  ts2gtopt
)

set(_SCRIPT_SOURCES)
set(_SCRIPT_TEST_DIRS)
set(_SCRIPT_COV_ARGS)
foreach(_mod IN LISTS _SCRIPT_MODULES)
  list(APPEND _SCRIPT_SOURCES   "${SCRIPTS_DIR}/${_mod}")
  list(APPEND _SCRIPT_COV_ARGS  "--cov=${SCRIPTS_DIR}/${_mod}")
endforeach()
# Add test dirs for packages that have tests/ subdirectories.
foreach(_mod IN ITEMS gtopt_compare cvs2parquet gtopt_diagram gtopt_field_extractor gtopt_check_lp gtopt_check_json gtopt_check_output gtopt_check_solvers gtopt_compress_lp gtopt_config gtopt2pp igtopt plp2gtopt pp2gtopt run_gtopt gtopt_monitor ts2gtopt)
  list(APPEND _SCRIPT_TEST_DIRS "${SCRIPTS_DIR}/${_mod}/tests")
endforeach()

# ---- Install runtime deps from requirements.txt ----

add_custom_target(scripts-pip-requirements
  COMMAND ${PYTHON_EXECUTABLE} -m pip install
    -r "${SCRIPTS_DIR}/requirements.txt"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Installing gtopt script runtime dependencies from requirements.txt"
)

# ---- Editable install (dev) ----

add_custom_target(scripts-install
  COMMAND ${PYTHON_EXECUTABLE} -m pip install -e "${SCRIPTS_DIR}[dev]"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Installing gtopt Python scripts in editable mode with dev dependencies"
)

# ---- Format (ruff format) ----

add_custom_target(scripts-format
  COMMAND ${PYTHON_EXECUTABLE} -m ruff format ${_SCRIPT_SOURCES}
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Auto-formatting Python source files with ruff format"
  DEPENDS scripts-install
)

# ---- Check format (ruff format --check) ----

add_custom_target(scripts-check-format
  COMMAND ${PYTHON_EXECUTABLE} -m ruff format --check ${_SCRIPT_SOURCES}
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Checking Python source formatting with ruff format (no files modified)"
  DEPENDS scripts-install
)

# ---- Lint (pylint) ----

add_custom_target(scripts-lint
  COMMAND ${PYTHON_EXECUTABLE} -m pylint
    --jobs=0
    ${_SCRIPT_MODULES}
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running pylint static analysis"
  DEPENDS scripts-install
)

# ---- Lint (ruff) ----

add_custom_target(scripts-ruff
  COMMAND ${PYTHON_EXECUTABLE} -m ruff check ${_SCRIPT_SOURCES}
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running ruff static analysis"
  DEPENDS scripts-install
)

# ---- Type check (mypy) ----

add_custom_target(scripts-mypy
  COMMAND ${PYTHON_EXECUTABLE} -m mypy
    ${_SCRIPT_MODULES}
    --ignore-missing-imports
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running mypy static type checking"
  DEPENDS scripts-install
)

# ---- Unit tests ----

add_custom_target(scripts-test
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    ${_SCRIPT_TEST_DIRS}
    -v -m "not integration"
    --ignore="${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running gtopt script unit tests"
  DEPENDS scripts-install
)

# ---- Integration tests (cmake build target) ----
# Runs all integration tests from plp2gtopt, igtopt, and ts2gtopt in one
# shot via pytest (serial; CTest handles parallelism via individual per-case
# targets).  For fine-grained, parallelisable per-case control use the CTest
# targets registered below (``ctest -L integration``).

add_custom_target(scripts-test-integration
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
    "${SCRIPTS_DIR}/igtopt/tests"
    "${SCRIPTS_DIR}/ts2gtopt/tests"
    -v -m integration
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running plp2gtopt, igtopt, and ts2gtopt integration tests"
  DEPENDS scripts-install
)

# ---- Coverage ----

add_custom_target(scripts-coverage
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    ${_SCRIPT_TEST_DIRS}
    -v
    ${_SCRIPT_COV_ARGS}
    --cov-report=html:${CMAKE_BINARY_DIR}/scripts-coverage-report
    --cov-report=term-missing
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running tests and generating coverage report"
  DEPENDS scripts-install
)

# ---- CTest integration ----

enable_testing()
include(TestParallelism)

# ---- Helper: add_pytest_integration_test ----
# Registers a pytest integration-test group as an individual CTest entry.
# Analogous to add_e2e_case() in integration_test/cmake/add_e2e_case.cmake.
include(${SCRIPTS_DIR}/cmake/add_pytest_integration_test.cmake)

# ---- Setup fixture ----
# Install dev dependencies before running any tests.
# This mirrors the DEPENDS scripts-install that the custom targets use,
# so that 'cmake --build <dir> --target test' works out of the box without
# requiring a prior manual 'pip install'.
add_test(
  NAME scripts-install-deps
  COMMAND ${CMAKE_COMMAND}
    -DSCRIPTS_DIR:PATH=${SCRIPTS_DIR}
    -DPYTHON_EXECUTABLE:FILEPATH=${PYTHON_EXECUTABLE}
    -P ${SCRIPTS_DIR}/cmake/scripts_install_deps.cmake
)
set_tests_properties(scripts-install-deps PROPERTIES
  FIXTURES_SETUP scripts-deps
)

# ---- Unit tests (one CTest entry per package) ----
# Tests are independent and run in parallel via CTest (``ctest -L unit``).

# Packages with no integration-marked tests: straightforward directory scan.
foreach(_pkg IN ITEMS gtopt_compare cvs2parquet gtopt_check_lp gtopt_check_json gtopt2pp pp2gtopt)
  add_pytest_unit_test(
    scripts-${_pkg}-unit
    DIR      "${SCRIPTS_DIR}/${_pkg}/tests"
    FIXTURE_REQUIRED scripts-deps
  )
endforeach()

# igtopt and ts2gtopt contain @integration-marked tests; the function always
# passes -m "not integration" so they are filtered out automatically.
foreach(_pkg IN ITEMS igtopt ts2gtopt)
  add_pytest_unit_test(
    scripts-${_pkg}-unit
    DIR      "${SCRIPTS_DIR}/${_pkg}/tests"
    FIXTURE_REQUIRED scripts-deps
  )
endforeach()

# plp2gtopt: ignore the dedicated integration test file (separate from the
# @pytest.mark.integration marks used by igtopt/ts2gtopt).
add_pytest_unit_test(
  scripts-plp2gtopt-unit
  DIR      "${SCRIPTS_DIR}/plp2gtopt/tests"
  IGNORE   "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
  FIXTURE_REQUIRED scripts-deps
)

# gtopt_diagram: tests now live inside the package directory.
add_pytest_unit_test(
  scripts-gtopt_diagram-unit
  DIR      "${SCRIPTS_DIR}/gtopt_diagram/tests"
  FIXTURE_REQUIRED scripts-deps
)

# ---- plp2gtopt integration tests (one CTest test per case group) ----
# Each test runs the subset of @pytest.mark.integration tests matching the
# given -k keyword.  Tests are independent and run in parallel via CTest.

set(_plp2gtopt_test_file
  "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
)

foreach(_case IN ITEMS
    dat_ex
    min_1bus
    min_2bus
    min_bess
    min_battery
    min_ess
    min_hydro
    min_mance
    min_manli
    min_reservoir
)
  add_pytest_integration_test(
    scripts-plp2gtopt-${_case}
    FILE     "${_plp2gtopt_test_file}"
    KEYWORD  "${_case}"
    FIXTURE_REQUIRED scripts-deps
  )
endforeach()

# ---- igtopt integration tests (one CTest test per xlsx case) ----

set(_igtopt_test_file "${SCRIPTS_DIR}/igtopt/tests/test_igtopt.py")

foreach(_case IN ITEMS igtopt_ieee57b igtopt_bat4b24)
  # igtopt tests parse an Excel workbook and optionally invoke the gtopt
  # solver; they take longer than plp2gtopt tests, so a higher COST
  # schedules them earlier so other tests can overlap.
  add_pytest_integration_test(
    scripts-igtopt-${_case}
    FILE     "${_igtopt_test_file}"
    KEYWORD  "${_case}"
    FIXTURE_REQUIRED scripts-deps
    COST 50
  )
endforeach()

# Pass the gtopt binary path to the igtopt integration tests so they can
# run the solver without relying on the standard build-directory heuristics.
set(_igtopt_int_tests
  scripts-igtopt-igtopt_ieee57b
  scripts-igtopt-igtopt_bat4b24
)
if(TARGET gtoptStandalone)
  set_tests_properties(${_igtopt_int_tests} PROPERTIES
    ENVIRONMENT "GTOPT_BIN=$<TARGET_FILE:gtoptStandalone>"
  )
elseif(DEFINED GTOPT_BINARY_PATH AND GTOPT_BINARY_PATH)
  set_tests_properties(${_igtopt_int_tests} PROPERTIES
    ENVIRONMENT "GTOPT_BIN=${GTOPT_BINARY_PATH}"
  )
endif()

# ---- ts2gtopt integration tests (one CTest test per test class) ----

set(_ts2gtopt_test_file "${SCRIPTS_DIR}/ts2gtopt/tests/test_bat4b_annual.py")

add_pytest_integration_test(
  scripts-ts2gtopt-demand_projection
  FILE     "${_ts2gtopt_test_file}"
  KEYWORD  "TestDemandProjectionIntegration"
  FIXTURE_REQUIRED scripts-deps
)

add_pytest_integration_test(
  scripts-ts2gtopt-solar_projection
  FILE     "${_ts2gtopt_test_file}"
  KEYWORD  "TestSolarProjectionIntegration"
  FIXTURE_REQUIRED scripts-deps
)

add_pytest_integration_test(
  scripts-ts2gtopt-generator_pmax
  FILE     "${_ts2gtopt_test_file}"
  KEYWORD  "TestGeneratorPmaxProjectionIntegration"
  FIXTURE_REQUIRED scripts-deps
)

add_pytest_integration_test(
  scripts-ts2gtopt-write_files
  FILE     "${_ts2gtopt_test_file}"
  KEYWORD  "TestWriteFiles"
  FIXTURE_REQUIRED scripts-deps
)

# ---- Check-format and ruff CTest tests ----

# add_test(
#   NAME scripts-check-format
#   COMMAND ${PYTHON_EXECUTABLE} -m ruff format --check ${_SCRIPT_SOURCES}
#   WORKING_DIRECTORY "${SCRIPTS_DIR}"
# )

# ruff check is run by the CI lint job and locally via `ruff check .`
# add_test(
#   NAME scripts-ruff
#   COMMAND ${PYTHON_EXECUTABLE} -m ruff check ${_SCRIPT_SOURCES}
#   WORKING_DIRECTORY "${SCRIPTS_DIR}"
# )
#
# set_tests_properties(
#   scripts-ruff
#   PROPERTIES FIXTURES_REQUIRED scripts-deps
# )

# ---- Optional static-analysis CTest tests ----

option(
  ENABLE_SCRIPTS_STATIC_ANALYSIS
  "Register scripts-lint (pylint) and scripts-mypy (mypy) as CTest tests"
  OFF
)

if(ENABLE_SCRIPTS_STATIC_ANALYSIS)
  # pylint takes module names without the .py extension for single-file modules.
  add_test(
    NAME scripts-lint
    COMMAND ${PYTHON_EXECUTABLE} -m pylint
      --jobs=0
      ${_SCRIPT_MODULES}
    WORKING_DIRECTORY "${SCRIPTS_DIR}"
  )

  # mypy requires the .py extension for single-file modules.
  add_test(
    NAME scripts-mypy
    COMMAND ${PYTHON_EXECUTABLE} -m mypy
      ${_SCRIPT_MODULES}
      --ignore-missing-imports
    WORKING_DIRECTORY "${SCRIPTS_DIR}"
  )

  set_tests_properties(
    scripts-lint
    scripts-mypy
    PROPERTIES FIXTURES_REQUIRED scripts-deps
  )
endif() # ENABLE_SCRIPTS_STATIC_ANALYSIS

# ---- Install ----

install(CODE "
  message(STATUS \"Installing gtopt Python scripts via pip...\")

  # Use the Python found at configure time.  Its absolute path is baked in
  # here, so it is immune to the PATH-stripping that sudo applies by default.
  # Both plain 'cmake --install' and 'sudo cmake --install' therefore use the
  # same Python, producing a predictable installation target.
  message(STATUS \"Using Python: ${PYTHON_EXECUTABLE}\")

  # Copy the source tree into a temporary staging directory inside the cmake
  # binary directory, excluding any pre-existing *.egg-info directory.  A
  # root-owned egg-info (left by a prior 'sudo cmake --install') would
  # otherwise cause setuptools to fail when the install is re-run without
  # sudo.  The staging copy is removed after pip finishes.
  # --ignore-installed prevents pip from attempting to uninstall a previous
  # version that may reside in a root-owned location (e.g. /usr/local/lib/…)
  # and cannot be removed without sudo.
  set(_stage \"${CMAKE_BINARY_DIR}/pip_stage\")
  file(REMOVE_RECURSE \"\${_stage}\")
  file(COPY \"${SCRIPTS_DIR}/\" DESTINATION \"\${_stage}\"
    PATTERN \"*.egg-info\" EXCLUDE
    PATTERN \"__pycache__\" EXCLUDE
  )

  # Prefer uv when available: it is 10-100x faster than pip on cold installs
  # (Rust-based resolver + parallel downloads) and is pre-installed on
  # ubuntu-latest GitHub Actions runners.
  find_program(_UV_EXEC uv)
  if(_UV_EXEC)
    message(STATUS \"Using uv for install: \${_UV_EXEC}\")
    execute_process(
      COMMAND \${_UV_EXEC} pip install
        --python \"${PYTHON_EXECUTABLE}\"
        --system
        --prefix \"\${CMAKE_INSTALL_PREFIX}\"
        \"\${_stage}\"
      RESULT_VARIABLE _pip_rc
    )
  else()
    execute_process(
      COMMAND \"${PYTHON_EXECUTABLE}\" -m pip install
        --prefix \"\${CMAKE_INSTALL_PREFIX}\"
        --ignore-installed
        \"\${_stage}\"
      RESULT_VARIABLE _pip_rc
    )
  endif()

  file(REMOVE_RECURSE \"\${_stage}\")
  if(NOT _pip_rc EQUAL 0)
    message(FATAL_ERROR \"pip install failed (exit code \${_pip_rc})\")
  endif()
  message(STATUS \"Scripts installed to \${CMAKE_INSTALL_PREFIX}/bin: \"
    \"gtopt_compare, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt, ts2gtopt\")

  # Record the installed entry-point paths in a manifest file so that
  # `cmake --build build --target uninstall` can remove them.  pip installs
  # entry-points to \${CMAKE_INSTALL_PREFIX}/bin/ but does not write to
  # cmake's install_manifest.txt.  The cmake_uninstall.cmake script reads
  # this auxiliary manifest to fill that gap.
  set(_scripts_manifest \"${CMAKE_CURRENT_BINARY_DIR}/scripts_install_manifest.txt\")
  set(_entry_points
    gtopt_compare
    cvs2parquet
    gtopt_diagram
    igtopt
    plp2gtopt
    pp2gtopt
    ts2gtopt
  )
  file(WRITE \"\${_scripts_manifest}\" \"\")
  foreach(_ep \${_entry_points})
    set(_ep_path \"\${CMAKE_INSTALL_PREFIX}/bin/\${_ep}\")
    if(EXISTS \"\${_ep_path}\")
      file(APPEND \"\${_scripts_manifest}\" \"\${_ep_path}\\n\")
    endif()
  endforeach()
  message(STATUS \"Recorded installed script paths in \${_scripts_manifest}\")
")
