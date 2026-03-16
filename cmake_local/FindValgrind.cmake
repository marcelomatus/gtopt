#[=======================================================================[.rst:
FindValgrind
------------

Find the ``valgrind`` instrumentation framework and the companion
``callgrind_annotate`` report tool.

Imported targets
^^^^^^^^^^^^^^^^

``Valgrind::Valgrind``
  The ``valgrind`` executable.

``Valgrind::CallgrindAnnotate``
  The ``callgrind_annotate`` executable.

Result variables
^^^^^^^^^^^^^^^^

``VALGRIND_FOUND``
  ``TRUE`` if ``valgrind`` is found.

``VALGRIND_EXECUTABLE``
  Full path to the ``valgrind`` executable.

``CALLGRIND_ANNOTATE_EXECUTABLE``
  Full path to the ``callgrind_annotate`` executable (may be empty even
  when ``VALGRIND_FOUND`` is true on some distributions).

``VALGRIND_VERSION``
  Version string reported by ``valgrind --version`` (if available).

Hints
^^^^^

Set ``VALGRIND_ROOT`` to a directory prefix in which to search for valgrind.

Install instructions
^^^^^^^^^^^^^^^^^^^^

If not found, install with::

  sudo apt install valgrind        # Ubuntu/Debian
  brew install valgrind            # macOS (Homebrew, limited support)

#]=======================================================================]

include_guard(GLOBAL)

find_program(
  VALGRIND_EXECUTABLE
  NAMES valgrind
  HINTS ${VALGRIND_ROOT}
  DOC "valgrind – instrumentation framework for dynamic analysis"
)

find_program(
  CALLGRIND_ANNOTATE_EXECUTABLE
  NAMES callgrind_annotate
  HINTS ${VALGRIND_ROOT}
  DOC "callgrind_annotate – callgrind profile report annotator"
)

if(VALGRIND_EXECUTABLE)
  execute_process(
    COMMAND "${VALGRIND_EXECUTABLE}" --version
    OUTPUT_VARIABLE _valgrind_version_output
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  string(REGEX MATCH "([0-9]+\\.[0-9]+\\.?[0-9]*)" VALGRIND_VERSION
         "${_valgrind_version_output}"
  )
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Valgrind
  REQUIRED_VARS VALGRIND_EXECUTABLE
  VERSION_VAR VALGRIND_VERSION
  FAIL_MESSAGE
    "valgrind not found. Install with: sudo apt install valgrind"
)

if(VALGRIND_FOUND)
  if(NOT TARGET Valgrind::Valgrind)
    add_executable(Valgrind::Valgrind IMPORTED)
    set_target_properties(
      Valgrind::Valgrind PROPERTIES IMPORTED_LOCATION "${VALGRIND_EXECUTABLE}"
    )
  endif()
  if(CALLGRIND_ANNOTATE_EXECUTABLE AND NOT TARGET Valgrind::CallgrindAnnotate)
    add_executable(Valgrind::CallgrindAnnotate IMPORTED)
    set_target_properties(
      Valgrind::CallgrindAnnotate PROPERTIES
      IMPORTED_LOCATION "${CALLGRIND_ANNOTATE_EXECUTABLE}"
    )
  endif()
endif()

mark_as_advanced(VALGRIND_EXECUTABLE CALLGRIND_ANNOTATE_EXECUTABLE VALGRIND_VERSION)
