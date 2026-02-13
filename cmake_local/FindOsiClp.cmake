include_guard(GLOBAL)

find_library(
  COIN_OSICLP_LIBRARY
  NAMES OsiClp libOsiClp
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiClp REQUIRED_VARS COIN_OSICLP_LIBRARY)

mark_as_advanced(COIN_OSICLP_LIBRARY)
