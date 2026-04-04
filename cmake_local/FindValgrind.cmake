#[=======================================================================[.rst:
FindValgrind
------------

Finds the ``valgrind`` executable.

Result variables
^^^^^^^^^^^^^^^^

``VALGRIND_EXECUTABLE`` — path to ``valgrind``
``Valgrind_FOUND``     — true if found

#]=======================================================================]

include_guard(GLOBAL)

find_program(VALGRIND_EXECUTABLE NAMES valgrind)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Valgrind
  REQUIRED_VARS VALGRIND_EXECUTABLE
)

if(NOT Valgrind_FOUND)
  message(FATAL_ERROR
    "valgrind not found. Install it with:\n"
    "  sudo apt-get install -y valgrind\n"
  )
endif()

mark_as_advanced(VALGRIND_EXECUTABLE)
