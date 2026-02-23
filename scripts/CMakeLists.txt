#[=======================================================================[.rst:
Scripts
-------

CMake configuration for installing and testing the gtopt Python scripts:
cvs2parquet, igtopt, and plp2gtopt.

The scripts are packaged as a standard Python project (pyproject.toml at the
repository root) so the usual ``pip install`` workflow applies.

Build targets
^^^^^^^^^^^^^

``scripts-install``
  Installs the Python package (all three scripts + their dependencies) into
  the active Python environment using ``pip install -e .[dev]``.

``scripts-test``
  Runs the plp2gtopt unit-test suite with pytest.

``scripts-test-integration``
  Runs the integration tests (marked ``integration``) using the sample PLP
  cases in ``scripts/cases/``.

``scripts-coverage``
  Runs unit + integration tests and generates an HTML coverage report under
  ``scripts-coverage-report/``.

Installation
^^^^^^^^^^^^

To install the scripts system-wide::

  cmake -S scripts -B build-scripts
  cmake --install build-scripts --prefix /usr/local

This installs the three console scripts (``cvs2parquet``, ``igtopt``,
``plp2gtopt``) to ``bin/`` via pip.

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

# Root of the repository (one level above this scripts/ directory)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ---- Install target ----

add_custom_target(scripts-install
  COMMAND ${PYTHON_EXECUTABLE} -m pip install -e "${REPO_ROOT}[dev]"
  WORKING_DIRECTORY "${REPO_ROOT}"
  COMMENT "Installing gtopt Python scripts and dev dependencies"
)

# ---- Unit tests ----

add_custom_target(scripts-test
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt/tests"
    -v --ignore="${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt/tests/test_integration.py"
  WORKING_DIRECTORY "${REPO_ROOT}"
  COMMENT "Running plp2gtopt unit tests"
  DEPENDS scripts-install
)

# ---- Integration tests ----

add_custom_target(scripts-test-integration
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt/tests/test_integration.py"
    -v -m integration
  WORKING_DIRECTORY "${REPO_ROOT}"
  COMMENT "Running plp2gtopt integration tests"
  DEPENDS scripts-install
)

# ---- Coverage ----

add_custom_target(scripts-coverage
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt/tests"
    -v
    --cov="${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt"
    --cov="${CMAKE_CURRENT_SOURCE_DIR}/igtopt"
    --cov="${CMAKE_CURRENT_SOURCE_DIR}/cvs2parquet"
    --cov-report=html:${CMAKE_BINARY_DIR}/scripts-coverage-report
    --cov-report=term-missing
  WORKING_DIRECTORY "${REPO_ROOT}"
  COMMENT "Running tests and generating coverage report"
  DEPENDS scripts-install
)

# ---- CTest integration ----

enable_testing()

add_test(
  NAME scripts-unit-tests
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt/tests"
    -v --ignore="${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt/tests/test_integration.py"
  WORKING_DIRECTORY "${REPO_ROOT}"
)

add_test(
  NAME scripts-integration-tests
  COMMAND ${PYTHON_EXECUTABLE} -m pytest
    "${CMAKE_CURRENT_SOURCE_DIR}/plp2gtopt/tests/test_integration.py"
    -v -m integration
  WORKING_DIRECTORY "${REPO_ROOT}"
)

# ---- Install ----

install(CODE "
  message(STATUS \"Installing gtopt Python scripts via pip...\")
  execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -m pip install
      --prefix \"\${CMAKE_INSTALL_PREFIX}\"
      \"${REPO_ROOT}\"
    RESULT_VARIABLE _pip_rc
  )
  if(NOT _pip_rc EQUAL 0)
    message(FATAL_ERROR \"pip install failed (exit code \${_pip_rc})\")
  endif()
  message(STATUS \"Scripts installed: cvs2parquet, igtopt, plp2gtopt\")
")
