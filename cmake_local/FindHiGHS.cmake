#[=======================================================================[.rst:
FindHiGHS
---------

Find the HiGHS LP/MIP solver.

Imported variables
^^^^^^^^^^^^^^^^^^

``HIGHS_FOUND``
  True if HiGHS was found.

``HIGHS_INCLUDE_DIRS``
  Include directories.

``HIGHS_LIBRARIES``
  Libraries to link against.

Cache variables
^^^^^^^^^^^^^^^

``HIGHS_ROOT_DIR``
  Hint for the HiGHS installation prefix.

#]=======================================================================]

include_guard(GLOBAL)

set(HIGHS_ROOT_DIR
    "/opt/highs"
    CACHE PATH "HiGHS installation prefix"
)

find_path(
  HIGHS_INCLUDE_DIR Highs.h
  HINTS ${HIGHS_ROOT_DIR}/include/highs ${HIGHS_ROOT_DIR}/include
  PATHS ENV C_INCLUDE_PATH ENV C_PLUS_INCLUDE_PATH ENV INCLUDE_PATH
        /usr/local/include/highs /usr/include/highs
)

find_library(
  HIGHS_LIBRARY
  NAMES highs
  HINTS ${HIGHS_ROOT_DIR}/lib
  PATHS ENV LIBRARY_PATH ENV LD_LIBRARY_PATH
        /usr/local/lib /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  HiGHS
  REQUIRED_VARS HIGHS_LIBRARY HIGHS_INCLUDE_DIR
)

if(HIGHS_FOUND)
  set(HIGHS_INCLUDE_DIRS ${HIGHS_INCLUDE_DIR})
  set(HIGHS_LIBRARIES ${HIGHS_LIBRARY})
endif()

mark_as_advanced(HIGHS_INCLUDE_DIR HIGHS_LIBRARY)
