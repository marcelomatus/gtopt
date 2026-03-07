#[=======================================================================[.rst:
Scripts
-------

CMake configuration for installing and testing the gtopt Python scripts:
compare_pandapower, cvs2parquet, gtopt-diagram, igtopt, plp2gtopt, pp2gtopt, and ts2gtopt.

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
  Run pylint over compare_pandapower, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt, and ts2gtopt;
  exit non-zero on any error/warning.  Registered as a CTest test only when
  ``-DENABLE_SCRIPTS_STATIC_ANALYSIS=ON`` is set (default: OFF).

``scripts-ruff``
  Run ruff check over compare_pandapower, cvs2parquet, gtopt_diagram.py, igtopt, plp2gtopt, pp2gtopt, and ts2gtopt;
  exit non-zero on any lint violation.

``scripts-mypy``
  Run mypy static type checking; exit non-zero on any type error.  Registered as
  a CTest test only when ``-DENABLE_SCRIPTS_STATIC_ANALYSIS=ON`` is set
  (default: OFF).

``scripts-test``
  Run the full unit-test suite (compare_pandapower, cvs2parquet, gtopt_diagram, igtopt, plp2gtopt, pp2gtopt,
  ts2gtopt) with pytest.

``scripts-test-integration``
  Run the integration tests (marked ``integration``) using the sample PLP
  cases in ``scripts/cases/`` and the igtopt Excel-based integration tests
  (requires a ``gtopt`` binary; tests are skipped when the binary is not found).

CTest / ``cmake --build <dir> --target test``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Running ``cmake --build <dir> --target test`` (or ``ctest`` inside the build
directory) invokes all registered tests.  A ``scripts-install-deps`` fixture
runs first and performs the equivalent of ``pip install -e scripts/[dev]`` from
an internal staging copy (excluding ``*.egg-info``) so pytest, ruff, pylint, and
mypy are available even in a fresh environment. All other tests declare
``FIXTURES_REQUIRED scripts-deps`` so they will be skipped if the setup fixture
fails.

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

# Default to ~/.local for non-root users (XDG Base Directory)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake_local/DefaultInstallPrefix.cmake")

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
  COMMAND ${PYTHON_EXECUTABLE} -m ruff format
    "${SCRIPTS_DIR}/compare_pandapower"
    "${SCRIPTS_DIR}/cvs2parquet"
    "${SCRIPTS_DIR}/gtopt_diagram.py"
    "${SCRIPTS_DIR}/igtopt"
    "${SCRIPTS_DIR}/plp2gtopt"
    "${SCRIPTS_DIR}/pp2gtopt"
    "${SCRIPTS_DIR}/ts2gtopt"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Auto-formatting Python source files with ruff format"
  DEPENDS scripts-install
)

# ---- Check format (ruff format --check) ----

add_custom_target(scripts-check-format
  COMMAND ${PYTHON_EXECUTABLE} -m ruff format --check
    "${SCRIPTS_DIR}/compare_pandapower"
    "${SCRIPTS_DIR}/cvs2parquet"
    "${SCRIPTS_DIR}/gtopt_diagram.py"
    "${SCRIPTS_DIR}/igtopt"
    "${SCRIPTS_DIR}/plp2gtopt"
    "${SCRIPTS_DIR}/pp2gtopt"
    "${SCRIPTS_DIR}/ts2gtopt"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Checking Python source formatting with ruff format (no files modified)"
  DEPENDS scripts-install
)

# ---- Lint (pylint) ----

add_custom_target(scripts-lint
  COMMAND ${PYTHON_EXECUTABLE} -m pylint
    --jobs=0
    compare_pandapower
    cvs2parquet
    gtopt_diagram
    igtopt
    plp2gtopt
    pp2gtopt
    ts2gtopt
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running pylint static analysis"
  DEPENDS scripts-install
)

# ---- Lint (ruff) ----

add_custom_target(scripts-ruff
  COMMAND ${PYTHON_EXECUTABLE} -m ruff check
    "${SCRIPTS_DIR}/compare_pandapower"
    "${SCRIPTS_DIR}/cvs2parquet"
    "${SCRIPTS_DIR}/gtopt_diagram.py"
    "${SCRIPTS_DIR}/igtopt"
    "${SCRIPTS_DIR}/plp2gtopt"
    "${SCRIPTS_DIR}/pp2gtopt"
    "${SCRIPTS_DIR}/ts2gtopt"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running ruff static analysis"
  DEPENDS scripts-install
)

# ---- Type check (mypy) ----

add_custom_target(scripts-mypy
  COMMAND ${PYTHON_EXECUTABLE} -m mypy
    compare_pandapower
    cvs2parquet
    gtopt_diagram.py
    igtopt
    plp2gtopt
    pp2gtopt
    ts2gtopt
    --ignore-missing-imports
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running mypy static type checking"
  DEPENDS scripts-install
)

# ---- Unit tests ----

add_custom_target(scripts-test
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/compare_pandapower/tests"
    "${SCRIPTS_DIR}/cvs2parquet/tests"
    "${SCRIPTS_DIR}/gtopt_diagram_tests"
    "${SCRIPTS_DIR}/igtopt/tests"
    "${SCRIPTS_DIR}/plp2gtopt/tests"
    "${SCRIPTS_DIR}/pp2gtopt/tests"
    "${SCRIPTS_DIR}/ts2gtopt/tests"
    -n auto
    -v -m "not integration"
    --ignore="${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running gtopt script unit tests"
  DEPENDS scripts-install
)

# ---- Integration tests ----

add_custom_target(scripts-test-integration
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
    "${SCRIPTS_DIR}/igtopt/tests"
    -n auto
    -v -m integration
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running plp2gtopt and igtopt integration tests"
  DEPENDS scripts-install
)

# ---- Coverage ----

add_custom_target(scripts-coverage
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/compare_pandapower/tests"
    "${SCRIPTS_DIR}/cvs2parquet/tests"
    "${SCRIPTS_DIR}/gtopt_diagram_tests"
    "${SCRIPTS_DIR}/igtopt/tests"
    "${SCRIPTS_DIR}/plp2gtopt/tests"
    "${SCRIPTS_DIR}/pp2gtopt/tests"
    "${SCRIPTS_DIR}/ts2gtopt/tests"
    -n auto
    -v
    --cov="${SCRIPTS_DIR}/compare_pandapower"
    --cov="${SCRIPTS_DIR}/cvs2parquet"
    --cov="${SCRIPTS_DIR}/gtopt_diagram.py"
    --cov="${SCRIPTS_DIR}/igtopt"
    --cov="${SCRIPTS_DIR}/plp2gtopt"
    --cov="${SCRIPTS_DIR}/pp2gtopt"
    --cov="${SCRIPTS_DIR}/ts2gtopt"
    --cov-report=html:${CMAKE_BINARY_DIR}/scripts-coverage-report
    --cov-report=term-missing
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running tests and generating coverage report"
  DEPENDS scripts-install
)

# ---- CTest integration ----

enable_testing()

include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
  set(NPROC 1)
endif()
# Leave at least one CPU free for other tests running alongside the expensive
# integration suite.  Clamped to 1 to ensure single-core machines can still
# run tests (NPROC_M1 must never be 0).
math(EXPR NPROC_M1 "${NPROC} - 1")
if(NPROC_M1 LESS 1)
  set(NPROC_M1 1)
endif()
list(APPEND CMAKE_CTEST_ARGUMENTS "--parallel" "${NPROC}")

# Setup fixture: install dev dependencies before running any tests.
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

add_test(
  NAME scripts-unit-tests
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/compare_pandapower/tests"
    "${SCRIPTS_DIR}/cvs2parquet/tests"
    "${SCRIPTS_DIR}/gtopt_diagram_tests"
    "${SCRIPTS_DIR}/igtopt/tests"
    "${SCRIPTS_DIR}/plp2gtopt/tests"
    "${SCRIPTS_DIR}/pp2gtopt/tests"
    "${SCRIPTS_DIR}/ts2gtopt/tests"
    -n auto
    -q --tb=short -m "not integration"
    --ignore="${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
)

add_test(
  NAME scripts-integration-tests
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
    "${SCRIPTS_DIR}/igtopt/tests"
    -n ${NPROC_M1}
    -v -m integration
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
)

add_test(
  NAME scripts-check-format
  COMMAND ${PYTHON_EXECUTABLE} -m ruff format --check
    "${SCRIPTS_DIR}/compare_pandapower"
    "${SCRIPTS_DIR}/cvs2parquet"
    "${SCRIPTS_DIR}/gtopt_diagram.py"
    "${SCRIPTS_DIR}/igtopt"
    "${SCRIPTS_DIR}/plp2gtopt"
    "${SCRIPTS_DIR}/pp2gtopt"
    "${SCRIPTS_DIR}/ts2gtopt"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
)

option(
  ENABLE_SCRIPTS_STATIC_ANALYSIS
  "Register scripts-lint (pylint) and scripts-mypy (mypy) as CTest tests"
  OFF
)

if(ENABLE_SCRIPTS_STATIC_ANALYSIS)
  add_test(
    NAME scripts-lint
    COMMAND ${PYTHON_EXECUTABLE} -m pylint
      --jobs=0
      compare_pandapower
      cvs2parquet
      gtopt_diagram
      igtopt
      plp2gtopt
      pp2gtopt
      ts2gtopt
    WORKING_DIRECTORY "${SCRIPTS_DIR}"
  )

  add_test(
    NAME scripts-mypy
    COMMAND ${PYTHON_EXECUTABLE} -m mypy
      compare_pandapower
      cvs2parquet
      gtopt_diagram.py
      igtopt
      plp2gtopt
      pp2gtopt
      ts2gtopt
      --ignore-missing-imports
    WORKING_DIRECTORY "${SCRIPTS_DIR}"
  )

  set_tests_properties(
    scripts-lint
    scripts-mypy
    PROPERTIES FIXTURES_REQUIRED scripts-deps
  )
endif() # ENABLE_SCRIPTS_STATIC_ANALYSIS

add_test(
  NAME scripts-ruff
  COMMAND ${PYTHON_EXECUTABLE} -m ruff check
    "${SCRIPTS_DIR}/compare_pandapower"
    "${SCRIPTS_DIR}/cvs2parquet"
    "${SCRIPTS_DIR}/gtopt_diagram.py"
    "${SCRIPTS_DIR}/igtopt"
    "${SCRIPTS_DIR}/plp2gtopt"
    "${SCRIPTS_DIR}/pp2gtopt"
    "${SCRIPTS_DIR}/ts2gtopt"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
)

set_tests_properties(
  scripts-unit-tests
  scripts-integration-tests
  scripts-check-format
  scripts-ruff
  PROPERTIES FIXTURES_REQUIRED scripts-deps
)

# Integration tests are long-running: schedule them first so other tests can
# overlap with them, and declare that they use NPROC-1 processor slots so
# CTest's scheduler avoids oversubscribing the CPU when the test runs
# internally with -n ${NPROC_M1} (NPROC-1) pytest-xdist workers.
set_tests_properties(scripts-integration-tests PROPERTIES
  COST 100.0
  PROCESSORS ${NPROC_M1}
)

# When the gtopt standalone binary is being built in the same cmake invocation
# (e.g. cmake -S all -B build), pass its path to the igtopt integration tests
# via the GTOPT_BIN environment variable so they can find the binary without
# relying on the standard build-directory search heuristics.
if(TARGET gtoptStandalone)
  set_tests_properties(scripts-integration-tests PROPERTIES
    ENVIRONMENT "GTOPT_BIN=$<TARGET_FILE:gtoptStandalone>"
  )
elseif(DEFINED GTOPT_BINARY_PATH AND GTOPT_BINARY_PATH)
  set_tests_properties(scripts-integration-tests PROPERTIES
    ENVIRONMENT "GTOPT_BIN=${GTOPT_BINARY_PATH}"
  )
endif()

# Unit tests run internally with -n auto (all CPUs via pytest-xdist).
# Declaring PROCESSORS ${NPROC} prevents CTest from scheduling another
# CPU-saturating test alongside them.
set_tests_properties(scripts-unit-tests PROPERTIES
  PROCESSORS ${NPROC}
)

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
    \"compare_pandapower, cvs2parquet, gtopt-diagram, igtopt, plp2gtopt, pp2gtopt, ts2gtopt\")

  # Record the installed entry-point paths in a manifest file so that
  # `cmake --build build --target uninstall` can remove them.  pip installs
  # entry-points to \${CMAKE_INSTALL_PREFIX}/bin/ but does not write to
  # cmake's install_manifest.txt.  The cmake_uninstall.cmake script reads
  # this auxiliary manifest to fill that gap.
  set(_scripts_manifest \"${CMAKE_CURRENT_BINARY_DIR}/scripts_install_manifest.txt\")
  set(_entry_points
    compare_pandapower
    cvs2parquet
    gtopt-diagram
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
