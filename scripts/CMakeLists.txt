#[=======================================================================[.rst:
Scripts
-------

CMake configuration for installing and testing the gtopt Python scripts:
cvs2parquet, igtopt, and plp2gtopt.

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

``scripts-test``
  Run the full unit-test suite (cvs2parquet, igtopt, plp2gtopt) with pytest.

``scripts-test-integration``
  Run the integration tests (marked ``integration``) using the sample PLP
  cases in ``scripts/cases/``.

``scripts-coverage``
  Run unit + integration tests and generate an HTML coverage report under
  ``<build>/scripts-coverage-report/``.

Installation
^^^^^^^^^^^^

To install the scripts system-wide alongside the gtopt binary::

  cmake -S scripts -B build-scripts
  cmake --install build-scripts --prefix /usr/local

This registers the three console scripts (``cvs2parquet``, ``igtopt``,
``plp2gtopt``) in ``<prefix>/bin/`` via pip.

#]=======================================================================]

cmake_minimum_required(VERSION 3.15)

project(gtoptScripts LANGUAGES NONE)

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

# ---- Unit tests ----

add_custom_target(scripts-test
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/cvs2parquet/tests"
    "${SCRIPTS_DIR}/igtopt/tests"
    "${SCRIPTS_DIR}/plp2gtopt/tests"
    -v --ignore="${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running gtopt script unit tests"
  DEPENDS scripts-install
)

# ---- Integration tests ----

add_custom_target(scripts-test-integration
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
    -v -m integration
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running plp2gtopt integration tests"
  DEPENDS scripts-install
)

# ---- Coverage ----

add_custom_target(scripts-coverage
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/cvs2parquet/tests"
    "${SCRIPTS_DIR}/igtopt/tests"
    "${SCRIPTS_DIR}/plp2gtopt/tests"
    -v
    --cov="${SCRIPTS_DIR}/cvs2parquet"
    --cov="${SCRIPTS_DIR}/igtopt"
    --cov="${SCRIPTS_DIR}/plp2gtopt"
    --cov-report=html:${CMAKE_BINARY_DIR}/scripts-coverage-report
    --cov-report=term-missing
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
  COMMENT "Running tests and generating coverage report"
  DEPENDS scripts-install
)

# ---- CTest integration ----

enable_testing()

add_test(
  NAME scripts-unit-tests
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/cvs2parquet/tests"
    "${SCRIPTS_DIR}/igtopt/tests"
    "${SCRIPTS_DIR}/plp2gtopt/tests"
    -v --ignore="${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
)

add_test(
  NAME scripts-integration-tests
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
    -v -m integration
  WORKING_DIRECTORY "${SCRIPTS_DIR}"
)

# ---- Install ----

install(CODE "
  message(STATUS \"Installing gtopt Python scripts via pip...\")
  execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -m pip install
      --prefix \"\${CMAKE_INSTALL_PREFIX}\"
      \"${SCRIPTS_DIR}\"
    RESULT_VARIABLE _pip_rc
  )
  if(NOT _pip_rc EQUAL 0)
    message(FATAL_ERROR \"pip install failed (exit code \${_pip_rc})\")
  endif()
  message(STATUS \"Scripts installed: cvs2parquet, igtopt, plp2gtopt\")
")
