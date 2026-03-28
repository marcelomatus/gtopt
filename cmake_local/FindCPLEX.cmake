#[=======================================================================[.rst:
FindCPLEX
---------

Find the IBM ILOG CPLEX Optimizer.

Imported variables
^^^^^^^^^^^^^^^^^^

``CPLEX_FOUND``
  True if CPLEX was found.

``CPLEX_INCLUDE_DIRS``
  Include directories.

``CPLEX_LIBRARIES``
  Libraries to link against.

``CPLEX_VERSION``
  Version string extracted from cpxconst.h (e.g. "22.1.1").

Cache variables
^^^^^^^^^^^^^^^

``CPLEX_ROOT_DIR``
  Hint for the CPLEX installation prefix.

#]=======================================================================]

include_guard(GLOBAL)

set(CPLEX_ROOT_DIR
    "/opt/cplex"
    CACHE PATH "IBM ILOG CPLEX installation prefix"
)

find_path(
  CPLEX_INCLUDE_DIR ilcplex/cplex.h
  HINTS ${CPLEX_ROOT_DIR}/include
  PATHS ENV CPLEX_HOME ENV C_INCLUDE_PATH ENV C_PLUS_INCLUDE_PATH
)

# CPLEX library layout: lib/<arch>/static_pic/libcplex.a
# Detect the host architecture subdirectory automatically.
set(_cplex_arch_hints
    ${CPLEX_ROOT_DIR}/lib/x86-64_linux/static_pic
    ${CPLEX_ROOT_DIR}/lib/x86_linux/static_pic
    ${CPLEX_ROOT_DIR}/lib/arm64_linux/static_pic
    ${CPLEX_ROOT_DIR}/lib/aarch64_linux/static_pic
    ${CPLEX_ROOT_DIR}/lib/ppc64le_linux/static_pic
    ${CPLEX_ROOT_DIR}/lib  # fallback: flat layout or dynamic lib
)

find_library(
  CPLEX_LIBRARY
  NAMES cplex
  HINTS ${_cplex_arch_hints}
  PATHS ENV LIBRARY_PATH ENV LD_LIBRARY_PATH
)

# Extract version from CPX_VERSION macro (format VVRRMMFF).
if(CPLEX_INCLUDE_DIR)
  file(STRINGS "${CPLEX_INCLUDE_DIR}/ilcplex/cpxconst.h" _cpx_ver_line
       REGEX "#define[ \t]+CPX_VERSION[ \t]+"
  )
  if(_cpx_ver_line MATCHES "#define[ \t]+CPX_VERSION[ \t]+([0-9]+)")
    math(EXPR _cpx_major "${CMAKE_MATCH_1} / 1000000")
    math(EXPR _cpx_minor "(${CMAKE_MATCH_1} / 10000) % 100")
    math(EXPR _cpx_patch "(${CMAKE_MATCH_1} / 100) % 100")
    set(CPLEX_VERSION "${_cpx_major}.${_cpx_minor}.${_cpx_patch}")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  CPLEX
  REQUIRED_VARS CPLEX_LIBRARY CPLEX_INCLUDE_DIR
  VERSION_VAR CPLEX_VERSION
)

if(CPLEX_FOUND)
  set(CPLEX_INCLUDE_DIRS ${CPLEX_INCLUDE_DIR})
  set(CPLEX_LIBRARIES ${CPLEX_LIBRARY})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPLEX_LIBRARIES m pthread)
  endif()
endif()

mark_as_advanced(CPLEX_INCLUDE_DIR CPLEX_LIBRARY)
