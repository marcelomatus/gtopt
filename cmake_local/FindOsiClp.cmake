find_library(
  COIN_OSICLP_LIBRARY
  NAMES OsiClp libOsiClp
  HINTS ${COIN_ROOT_DIR}/lib/coin
  HINTS ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiClp DEFAULT_MSG COIN_OSICLP_LIBRARY)
