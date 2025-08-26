# This module finds cplex.
#
# User can give CPLEX_ROOT_DIR as a hint stored in the cmake cache.
#
# It sets the following variables: CPLEX_FOUND              - Set to false, or
# undefined, if cplex isn't found. CPLEX_INCLUDE_DIRS       - include directory
# CPLEX_LIBRARIES          - library files

# config
set(CPLEX_ROOT_DIR
    "/opt/cplex"
    CACHE PATH "CPLEX root directory"
)

message(STATUS "CPLEX Root: ${CPLEX_ROOT_DIR}")

# cplex root dir guessing windows: trying to guess the root dir from a env variable
# set by the cplex installer

find_path(
  CPLEX_INCLUDE_DIR cplex.h
  HINTS ${CPLEX_ROOT_DIR}/include/ilcplex ${CPLEX_ROOT_DIR}/include
  PATHS ENV C_INCLUDE_PATH ENV C_PLUS_INCLUDE_PATH ENV INCLUDE_PATH
)
message(STATUS "CPLEX Include: ${CPLEX_INCLUDE_DIR}")

find_library(
  CPLEX_LIBRARY
  NAMES cplex
  HINTS ${CPLEX_ROOT_DIR}/lib/x86-64_linux/static_pic # unix
        ${CPLEX_ROOT_DIR}/lib/x86_linux/static_pic # unix
  PATHS ENV LIBRARY_PATH # unix
        ENV LD_LIBRARY_PATH # unix
)
message(STATUS "CPLEX Library: ${CPLEX_LIBRARY}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CPLEX DEFAULT_MSG CPLEX_LIBRARY CPLEX_INCLUDE_DIR)

find_path(
  CPLEX_BIN_DIR cplex
  HINTS ${CPLEX_ROOT_DIR}/bin/x86-64_linux # unix
        ${CPLEX_ROOT_DIR}/bin/x86_linux # unix
        PATH
        ENV
        PATH
        ENV
        LIBRARY_PATH
        ENV
        LD_LIBRARY_PATH
)
message(STATUS "CPLEX Bin Dir: ${CPLEX_BIN_DIR}")

if(CPLEX_FOUND)
  set(CPLEX_INCLUDE_DIRS ${CPLEX_INCLUDE_DIR})
  set(CPLEX_LIBRARIES ${CPLEX_LIBRARY})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CPLEX_LIBRARIES "${CPLEX_LIBRARIES};m;pthread")
  endif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
endif(CPLEX_FOUND)

mark_as_advanced(CPLEX_LIBRARY CPLEX_INCLUDE_DIR CPLEX_BIN_DIR)
