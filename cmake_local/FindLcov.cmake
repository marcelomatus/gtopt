#[=======================================================================[.rst:
FindLcov
--------

Finds the ``lcov`` and ``genhtml`` executables.

If not found, prints the install command and stops with a fatal error.

Result variables
^^^^^^^^^^^^^^^^

``LCOV_EXECUTABLE``    — path to ``lcov``
``GENHTML_EXECUTABLE`` — path to ``genhtml``
``Lcov_FOUND``         — true if both found

#]=======================================================================]

include_guard(GLOBAL)

find_program(LCOV_EXECUTABLE NAMES lcov)
find_program(GENHTML_EXECUTABLE NAMES genhtml)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Lcov
  REQUIRED_VARS LCOV_EXECUTABLE GENHTML_EXECUTABLE
)

if(NOT Lcov_FOUND)
  message(FATAL_ERROR
    "lcov not found. Install it with:\n"
    "  sudo apt-get install -y lcov\n"
  )
endif()

mark_as_advanced(LCOV_EXECUTABLE GENHTML_EXECUTABLE)
