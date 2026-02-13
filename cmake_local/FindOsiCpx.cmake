include_guard(GLOBAL)

find_library(
  COIN_OSICPX_LIBRARY
  NAMES OsiCpx libOsiCpx
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiCpx REQUIRED_VARS COIN_OSICPX_LIBRARY)

mark_as_advanced(COIN_OSICPX_LIBRARY)
