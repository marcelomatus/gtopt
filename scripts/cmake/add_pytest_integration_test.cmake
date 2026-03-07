#[=======================================================================[.rst:
AddPytestIntegrationTest
------------------------

Provides the ``add_pytest_integration_test`` function for registering
individual pytest integration-test groups as separate, parallelisable
CTest entries.

Each registered test runs a subset of ``@pytest.mark.integration`` tests
from a pytest file or directory, selected by a ``-k`` keyword filter.
Tests created this way can run in parallel via ``ctest --parallel`` and
are individually reportable — analogous to the ``add_e2e_case`` function
used in ``integration_test/cmake/add_e2e_case.cmake``.

Synopsis
^^^^^^^^

.. code-block:: cmake

   add_pytest_integration_test(
     <name>
     FILE     <path>
     KEYWORD  <keyword>
     [WORKING_DIRECTORY <dir>]
     [FIXTURE_REQUIRED  <fixture>]
     [ENVIRONMENT       <VAR=VALUE>...]
     [COST              <number>]
   )

Parameters
^^^^^^^^^^

``<name>``
  The CTest test name (positional first argument,
  e.g. ``scripts-plp2gtopt-min_1bus``).

``FILE``
  Absolute path to the pytest test file or directory.

``KEYWORD``
  The ``pytest -k`` keyword expression selecting the test group
  (e.g. ``min_1bus`` matches all ``test_min_1bus_*`` functions;
  class names like ``TestDemandProjectionIntegration`` select the
  corresponding test class).

``WORKING_DIRECTORY``
  Working directory when running the test.  Defaults to the value of
  the ``SCRIPTS_DIR`` variable when it is defined.

``FIXTURE_REQUIRED``
  Optional CTest fixture name that must succeed before this test runs
  (e.g. ``scripts-deps``).

``ENVIRONMENT``
  Optional list of ``VAR=VALUE`` pairs injected into the test environment.
  Multiple pairs can be specified as separate list items,
  e.g. ``ENVIRONMENT "GTOPT_BIN=/path/to/gtopt" "FOO=bar"``.

``COST``
  CTest scheduling hint — higher values are scheduled earlier, allowing
  long-running tests to overlap with shorter ones.  Default: ``10``.

Required variables
^^^^^^^^^^^^^^^^^^

``PYTHON_EXECUTABLE``
  Path to the Python interpreter found at configure time
  (set by ``find_program(PYTHON_EXECUTABLE ...)``).

``SCRIPTS_DIR``
  Root of the scripts source tree; used as the default working directory
  when ``WORKING_DIRECTORY`` is not specified.

Labels
^^^^^^

Every test registered by this function receives the CTest label
``integration``, enabling convenient bulk selection via
``ctest -L integration``.

Example
^^^^^^^

.. code-block:: cmake

   set(SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
   include(cmake/add_pytest_integration_test.cmake)

   # One CTest test per plp2gtopt integration case
   foreach(_case min_1bus min_2bus min_bess min_hydro)
     add_pytest_integration_test(
       scripts-plp2gtopt-${_case}
       FILE    "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
       KEYWORD "${_case}"
       FIXTURE_REQUIRED scripts-deps
     )
   endforeach()

#]=======================================================================]

include_guard(GLOBAL)

function(add_pytest_integration_test name)
  cmake_parse_arguments(
    PARSE_ARGV 1 ARG
    ""
    "FILE;KEYWORD;WORKING_DIRECTORY;FIXTURE_REQUIRED;COST"
    "ENVIRONMENT"
  )

  if(NOT ARG_FILE)
    message(FATAL_ERROR
      "add_pytest_integration_test(${name}): FILE is required")
  endif()

  if(NOT ARG_KEYWORD)
    message(FATAL_ERROR
      "add_pytest_integration_test(${name}): KEYWORD is required")
  endif()

  if(NOT DEFINED PYTHON_EXECUTABLE)
    message(FATAL_ERROR
      "add_pytest_integration_test: PYTHON_EXECUTABLE is not defined. "
      "Call find_program(PYTHON_EXECUTABLE ...) before including this module.")
  endif()

  if(NOT ARG_WORKING_DIRECTORY)
    if(DEFINED SCRIPTS_DIR)
      set(ARG_WORKING_DIRECTORY "${SCRIPTS_DIR}")
    else()
      message(FATAL_ERROR
        "add_pytest_integration_test(${name}): WORKING_DIRECTORY is required "
        "when SCRIPTS_DIR is not defined.")
    endif()
  endif()

  if(NOT ARG_COST)
    set(ARG_COST 10)
  endif()

  add_test(
    NAME "${name}"
    COMMAND "${PYTHON_EXECUTABLE}" -m pytest
      "${ARG_FILE}"
      -k "${ARG_KEYWORD}"
      -m integration
      -v --tb=short
    WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}"
  )

  set(_props
    COST "${ARG_COST}"
    LABELS integration
  )

  if(ARG_FIXTURE_REQUIRED)
    list(APPEND _props FIXTURES_REQUIRED "${ARG_FIXTURE_REQUIRED}")
  endif()

  if(ARG_ENVIRONMENT)
    list(APPEND _props ENVIRONMENT "${ARG_ENVIRONMENT}")
  endif()

  set_tests_properties("${name}" PROPERTIES ${_props})
endfunction()
