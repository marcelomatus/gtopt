include_guard(GLOBAL)

find_library(
  COIN_OSIHIGHS_LIBRARY
  NAMES OsiHiGHS libOsiHiGHS
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiHiGHS REQUIRED_VARS COIN_OSIHIGHS_LIBRARY)

mark_as_advanced(COIN_OSIHIGHS_LIBRARY)
