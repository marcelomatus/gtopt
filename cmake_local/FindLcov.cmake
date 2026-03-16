#[=======================================================================[.rst:
FindLcov
--------

Find the ``lcov`` code-coverage collection tool and its companion
``genhtml`` report generator.

Imported targets
^^^^^^^^^^^^^^^^

This module defines the following ``IMPORTED`` targets when found:

``Lcov::Lcov``
  The ``lcov`` executable.

``Lcov::Genhtml``
  The ``genhtml`` executable.

Result variables
^^^^^^^^^^^^^^^^

``LCOV_FOUND``
  ``TRUE`` if both ``lcov`` and ``genhtml`` are found.

``LCOV_EXECUTABLE``
  Full path to the ``lcov`` executable.

``GENHTML_EXECUTABLE``
  Full path to the ``genhtml`` executable.

``LCOV_VERSION``
  Version string reported by ``lcov --version`` (if available).

Hints
^^^^^

Set ``LCOV_ROOT`` to a directory prefix in which to search for lcov.

Install instructions
^^^^^^^^^^^^^^^^^^^^

If not found, install with::

  sudo apt install lcov           # Ubuntu/Debian
  brew install lcov               # macOS (Homebrew)

#]=======================================================================]

include_guard(GLOBAL)

find_program(
  LCOV_EXECUTABLE
  NAMES lcov
  HINTS ${LCOV_ROOT}
  DOC "lcov – Linux Test Project coverage tool"
)

find_program(
  GENHTML_EXECUTABLE
  NAMES genhtml
  HINTS ${LCOV_ROOT}
  DOC "genhtml – HTML coverage report generator (part of lcov)"
)

if(LCOV_EXECUTABLE)
  execute_process(
    COMMAND "${LCOV_EXECUTABLE}" --version
    OUTPUT_VARIABLE _lcov_version_output
    ERROR_VARIABLE _lcov_version_err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )
  string(REGEX MATCH "([0-9]+\\.[0-9]+)" LCOV_VERSION "${_lcov_version_output}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Lcov
  REQUIRED_VARS LCOV_EXECUTABLE GENHTML_EXECUTABLE
  VERSION_VAR LCOV_VERSION
  FAIL_MESSAGE
    "lcov/genhtml not found. Install with: sudo apt install lcov"
)

if(LCOV_FOUND)
  if(NOT TARGET Lcov::Lcov)
    add_executable(Lcov::Lcov IMPORTED)
    set_target_properties(Lcov::Lcov PROPERTIES IMPORTED_LOCATION "${LCOV_EXECUTABLE}")
  endif()
  if(NOT TARGET Lcov::Genhtml)
    add_executable(Lcov::Genhtml IMPORTED)
    set_target_properties(
      Lcov::Genhtml PROPERTIES IMPORTED_LOCATION "${GENHTML_EXECUTABLE}"
    )
  endif()
endif()

mark_as_advanced(LCOV_EXECUTABLE GENHTML_EXECUTABLE LCOV_VERSION)
