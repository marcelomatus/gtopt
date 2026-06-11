#[=======================================================================[.rst:
FindCuOpt
---------

Find the NVIDIA cuOpt GPU optimization solver (``libcuopt``).

cuOpt ships its native C++/C library inside the pip ``libcuopt-cu12``
wheel.  Unlike the other solvers, gtopt's plugin uses **only the stable
C API** declared in ``cuopt/linear_programming/cuopt_c.h`` — no CUDA
toolkit headers, no RAFT/RMM C++ templates — so a plain ``g++`` plugin
links against ``libcuopt.so`` directly (verified: the C API symbols
``cuOptCreateRangedProblem`` / ``cuOptSolve`` / ``cuOptGetDualSolution``
/ ``cuOptGetReducedCosts`` are exported from ``libcuopt.so``).

Search order for the include dir and library:

1. ``CUOPT_HOME`` environment variable      (root containing include/ + lib64/)
2. ``CUOPT_ROOT_DIR`` CMake variable
3. ``$ENV{CUOPT_ROOT_DIR}``
4. The pip wheel location (auto-detected via ``python -c "import libcuopt"``)
5. Common install prefixes (/opt/cuopt, /usr/local).

Result variables::

  ``CUOPT_FOUND``        — TRUE if cuOpt was found
  ``CUOPT_INCLUDE_DIRS`` — Header include directory (contains cuopt/)
  ``CUOPT_LIBRARIES``    — Libraries to link against (libcuopt.so)
  ``CUOPT_VERSION``      — Version string when discoverable (VERSION file)

Disable explicitly with ``-DGTOPT_DISABLE_CUOPT=ON``.

#]=======================================================================]

if(GTOPT_DISABLE_CUOPT)
  set(CUOPT_FOUND FALSE)
  return()
endif()

set(_CUOPT_SEARCH_PATHS)

if(DEFINED ENV{CUOPT_HOME})
  list(APPEND _CUOPT_SEARCH_PATHS "$ENV{CUOPT_HOME}")
endif()
if(CUOPT_ROOT_DIR)
  list(APPEND _CUOPT_SEARCH_PATHS "${CUOPT_ROOT_DIR}")
endif()
if(DEFINED ENV{CUOPT_ROOT_DIR})
  list(APPEND _CUOPT_SEARCH_PATHS "$ENV{CUOPT_ROOT_DIR}")
endif()

# Auto-detect the pip wheel install location.  ``libcuopt`` is a namespace
# package whose __file__ sits next to include/ and lib64/.
find_program(_CUOPT_PYTHON NAMES python3 python)
if(_CUOPT_PYTHON)
  execute_process(
    COMMAND "${_CUOPT_PYTHON}" -c
            "import libcuopt, os; print(os.path.dirname(libcuopt.__file__))"
    OUTPUT_VARIABLE _CUOPT_WHEEL_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _CUOPT_PY_RC
  )
  if(_CUOPT_PY_RC EQUAL 0 AND _CUOPT_WHEEL_DIR)
    list(APPEND _CUOPT_SEARCH_PATHS "${_CUOPT_WHEEL_DIR}")
  endif()
endif()

list(APPEND _CUOPT_SEARCH_PATHS "/opt/cuopt" "/usr/local")

find_path(CUOPT_INCLUDE_DIR
  NAMES cuopt/linear_programming/cuopt_c.h
  HINTS ${_CUOPT_SEARCH_PATHS}
  PATH_SUFFIXES include
)

find_library(CUOPT_LIBRARY
  NAMES cuopt
  HINTS ${_CUOPT_SEARCH_PATHS}
  PATH_SUFFIXES lib64 lib
)

# Best-effort version string from the wheel's VERSION file.
set(CUOPT_VERSION "")
if(_CUOPT_WHEEL_DIR AND EXISTS "${_CUOPT_WHEEL_DIR}/VERSION")
  file(READ "${_CUOPT_WHEEL_DIR}/VERSION" CUOPT_VERSION)
  string(STRIP "${CUOPT_VERSION}" CUOPT_VERSION)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CuOpt
  REQUIRED_VARS CUOPT_LIBRARY CUOPT_INCLUDE_DIR
  VERSION_VAR   CUOPT_VERSION
)

if(CUOPT_FOUND)
  set(CUOPT_INCLUDE_DIRS "${CUOPT_INCLUDE_DIR}")
  set(CUOPT_LIBRARIES    "${CUOPT_LIBRARY}")
  mark_as_advanced(CUOPT_INCLUDE_DIR CUOPT_LIBRARY)
endif()
