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
# No unconditional FATAL_ERROR here: valgrind is optional for callers
# like profiling/ (perf-only runs must still configure).  Callers that
# genuinely require it should use `find_package(Valgrind REQUIRED)`,
# which find_package_handle_standard_args turns into a hard error.
find_package_handle_standard_args(
  Valgrind
  REQUIRED_VARS VALGRIND_EXECUTABLE
)

mark_as_advanced(VALGRIND_EXECUTABLE)
