include_guard(GLOBAL)

find_library(
  COIN_OSICBC_LIBRARY
  NAMES OsiCbc libOsiCbc
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiCbc REQUIRED_VARS COIN_OSICBC_LIBRARY)

mark_as_advanced(COIN_OSICBC_LIBRARY)
