# This module finds cplex.
#
# User can give CPLEX_ROOT_DIR as a hint stored in the cmake cache.
#
# It sets the following variables:
#  CPLEX_FOUND              - Set to false, or undefined, if cplex isn't found.
#  CPLEX_INCLUDE_DIRS       - include directory
#  CPLEX_LIBRARIES          - library files

## config
set(CPLEX_ROOT_DIR "" CACHE PATH "CPLEX root directory")

message(STATUS "CPLEX Root: ${CPLEX_ROOT_DIR}")

## cplex root dir guessing
# windows: trying to guess the root dir from a 
# env variable set by the cplex installer

FIND_PATH(CPLEX_INCLUDE_DIR
  cplex.h
  HINTS ${CPLEX_ROOT_DIR}/include/ilcplex
        ${CPLEX_ROOT_DIR}/include
  PATHS ENV C_INCLUDE_PATH
        ENV C_PLUS_INCLUDE_PATH
        ENV INCLUDE_PATH
  )
message(STATUS "CPLEX Include: ${CPLEX_INCLUDE_DIR}")

FIND_LIBRARY(CPLEX_LIBRARY
  NAMES cplex
  HINTS ${CPLEX_ROOT_DIR}/lib/x86-64_linux/static_pic #unix
        ${CPLEX_ROOT_DIR}/lib/x86_linux/static_pic #unix 
  PATHS ENV LIBRARY_PATH #unix
        ENV LD_LIBRARY_PATH #unix
  )
message(STATUS "CPLEX Library: ${CPLEX_LIBRARY}")

INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(CPLEX DEFAULT_MSG 
 CPLEX_LIBRARY CPLEX_INCLUDE_DIR )
	
FIND_PATH(CPLEX_BIN_DIR
  cplex 
  HINTS ${CPLEX_ROOT_DIR}/bin/x86-64_linux #unix 
        ${CPLEX_ROOT_DIR}/bin/x86_linux #unix
  PATH ENV PATH
       ENV LIBRARY_PATH
       ENV LD_LIBRARY_PATH
  )
message(STATUS "CPLEX Bin Dir: ${CPLEX_BIN_DIR}")

IF(CPLEX_FOUND)
  SET(CPLEX_INCLUDE_DIRS ${CPLEX_INCLUDE_DIR})
  SET(CPLEX_LIBRARIES ${CPLEX_LIBRARY} )
  IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    SET(CPLEX_LIBRARIES "${CPLEX_LIBRARIES};m;pthread")
  ENDIF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
ENDIF(CPLEX_FOUND)

MARK_AS_ADVANCED(CPLEX_LIBRARY CPLEX_INCLUDE_DIR CPLEX_BIN_DIR)



