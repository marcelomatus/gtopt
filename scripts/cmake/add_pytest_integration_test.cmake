#[=======================================================================[.rst:
AddPytestTests
--------------

Provides two functions for registering individual pytest test groups as
separate, parallelisable CTest entries:

``add_pytest_integration_test``
  Registers a ``@pytest.mark.integration`` test group selected by a ``-k``
  keyword expression.  Every test registered by this function carries the
  CTest label ``integration``.

``add_pytest_unit_test``
  Registers a per-package unit test directory.  Integration-marked tests are
  automatically excluded (``-m "not integration"``).  Every test registered
  by this function carries the CTest label ``unit``.

Both functions are analogous to ``add_e2e_case`` used in
``integration_test/cmake/add_e2e_case.cmake``.

---------------------------------------------------------------------

``add_pytest_integration_test``
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Synopsis
""""""""

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
""""""""""

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
  Working directory when running the test.  Defaults to ``SCRIPTS_DIR``.

``FIXTURE_REQUIRED``
  Optional CTest fixture name that must succeed before this test runs.

``ENVIRONMENT``
  Optional list of ``VAR=VALUE`` pairs injected into the test environment.
  Multiple pairs can be specified as separate list items,
  e.g. ``ENVIRONMENT "GTOPT_BIN=/path/to/gtopt" "FOO=bar"``.

``COST``
  CTest scheduling hint — higher values are scheduled earlier.  Default: ``10``.

Example
"""""""

.. code-block:: cmake

   include(cmake/add_pytest_integration_test.cmake)
   foreach(_case min_1bus min_2bus min_bess min_hydro)
     add_pytest_integration_test(
       scripts-plp2gtopt-${_case}
       FILE    "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
       KEYWORD "${_case}"
       FIXTURE_REQUIRED scripts-deps
     )
   endforeach()

---------------------------------------------------------------------

``add_pytest_unit_test``
^^^^^^^^^^^^^^^^^^^^^^^^^

Synopsis
""""""""

.. code-block:: cmake

   add_pytest_unit_test(
     <name>
     DIR               <path>
     [WORKING_DIRECTORY <dir>]
     [FIXTURE_REQUIRED  <fixture>]
     [IGNORE            <file>...]
     [ENVIRONMENT       <VAR=VALUE>...]
     [COST              <number>]
   )

Parameters
""""""""""

``<name>``
  The CTest test name (e.g. ``scripts-plp2gtopt-unit``).

``DIR``
  Absolute path to the test directory to run.

``WORKING_DIRECTORY``
  Working directory when running the test.  Defaults to ``SCRIPTS_DIR``.

``FIXTURE_REQUIRED``
  Optional CTest fixture name that must succeed before this test runs.

``IGNORE``
  Optional list of file paths to pass as ``--ignore=<path>`` to pytest
  (e.g. to skip a dedicated integration-test file that lives inside the
  unit-test directory).

``ENVIRONMENT``
  Optional list of ``VAR=VALUE`` pairs injected into the test environment.

``COST``
  CTest scheduling hint.  Default: ``5``.

Example
"""""""

.. code-block:: cmake

   include(cmake/add_pytest_integration_test.cmake)

   add_pytest_unit_test(
     scripts-plp2gtopt-unit
     DIR      "${SCRIPTS_DIR}/plp2gtopt/tests"
     IGNORE   "${SCRIPTS_DIR}/plp2gtopt/tests/test_integration.py"
     FIXTURE_REQUIRED scripts-deps
   )

   add_pytest_unit_test(
     scripts-igtopt-unit
     DIR      "${SCRIPTS_DIR}/igtopt/tests"
     FIXTURE_REQUIRED scripts-deps
   )

---------------------------------------------------------------------

Required variables (both functions)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``PYTHON_EXECUTABLE``
  Path to the Python interpreter found at configure time.

``SCRIPTS_DIR``
  Root of the scripts source tree; used as the default working directory.

#]=======================================================================]

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# add_pytest_integration_test
# ---------------------------------------------------------------------------

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

# ---------------------------------------------------------------------------
# add_pytest_unit_test
# ---------------------------------------------------------------------------

function(add_pytest_unit_test name)
  cmake_parse_arguments(
    PARSE_ARGV 1 ARG
    ""
    "DIR;WORKING_DIRECTORY;FIXTURE_REQUIRED;COST"
    "IGNORE;ENVIRONMENT"
  )

  if(NOT ARG_DIR)
    message(FATAL_ERROR
      "add_pytest_unit_test(${name}): DIR is required")
  endif()

  if(NOT DEFINED PYTHON_EXECUTABLE)
    message(FATAL_ERROR
      "add_pytest_unit_test: PYTHON_EXECUTABLE is not defined. "
      "Call find_program(PYTHON_EXECUTABLE ...) before including this module.")
  endif()

  if(NOT ARG_WORKING_DIRECTORY)
    if(DEFINED SCRIPTS_DIR)
      set(ARG_WORKING_DIRECTORY "${SCRIPTS_DIR}")
    else()
      message(FATAL_ERROR
        "add_pytest_unit_test(${name}): WORKING_DIRECTORY is required "
        "when SCRIPTS_DIR is not defined.")
    endif()
  endif()

  if(NOT ARG_COST)
    set(ARG_COST 5)
  endif()

  # Build --ignore=<path> arguments for each ignored file/directory.
  set(_ignore_args)
  foreach(_ign IN LISTS ARG_IGNORE)
    list(APPEND _ignore_args "--ignore=${_ign}")
  endforeach()

  add_test(
    NAME "${name}"
    COMMAND "${PYTHON_EXECUTABLE}" -m pytest
      "${ARG_DIR}"
      -m "not integration"
      -v --tb=short
      ${_ignore_args}
    WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}"
  )

  set(_props
    COST "${ARG_COST}"
    LABELS unit
  )

  if(ARG_FIXTURE_REQUIRED)
    list(APPEND _props FIXTURES_REQUIRED "${ARG_FIXTURE_REQUIRED}")
  endif()

  if(ARG_ENVIRONMENT)
    list(APPEND _props ENVIRONMENT "${ARG_ENVIRONMENT}")
  endif()

  set_tests_properties("${name}" PROPERTIES ${_props})
endfunction()
