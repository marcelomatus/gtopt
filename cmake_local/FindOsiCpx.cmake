find_library(
  COIN_OSICPX_LIBRARY
  NAMES OsiCpx libOsiCpx
  HINTS ${COIN_ROOT_DIR}/lib/coin
  HINTS ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiCpx DEFAULT_MSG COIN_OSICPX_LIBRARY)
