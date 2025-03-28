find_library(
  COIN_OSICBC_LIBRARY
  NAMES OsiCbc libOsiCbc
  HINTS ${COIN_ROOT_DIR}/lib/coin
  HINTS ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiCbc DEFAULT_MSG COIN_OSICBC_LIBRARY)
