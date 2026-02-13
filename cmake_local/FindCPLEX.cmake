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

Cache variables
^^^^^^^^^^^^^^^

``CPLEX_ROOT_DIR``
  Hint for the CPLEX installation prefix.

#]=======================================================================]

include_guard(GLOBAL)

set(CPLEX_ROOT_DIR
    "/opt/cplex"
    CACHE PATH "CPLEX root directory"
)

find_path(
  CPLEX_INCLUDE_DIR cplex.h
  HINTS ${CPLEX_ROOT_DIR}/include/ilcplex ${CPLEX_ROOT_DIR}/include
  PATHS ENV C_INCLUDE_PATH ENV C_PLUS_INCLUDE_PATH ENV INCLUDE_PATH
)

find_library(
  CPLEX_LIBRARY
  NAMES cplex
  HINTS ${CPLEX_ROOT_DIR}/lib/x86-64_linux/static_pic
        ${CPLEX_ROOT_DIR}/lib/x86_linux/static_pic
  PATHS ENV LIBRARY_PATH ENV LD_LIBRARY_PATH
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  CPLEX
  REQUIRED_VARS CPLEX_LIBRARY CPLEX_INCLUDE_DIR
)

if(CPLEX_FOUND)
  set(CPLEX_INCLUDE_DIRS ${CPLEX_INCLUDE_DIR})
  set(CPLEX_LIBRARIES ${CPLEX_LIBRARY})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPLEX_LIBRARIES "${CPLEX_LIBRARIES};m;pthread")
  endif()
endif()

mark_as_advanced(CPLEX_INCLUDE_DIR CPLEX_LIBRARY)
