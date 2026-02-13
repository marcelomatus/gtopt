include_guard(GLOBAL)

find_library(
  COIN_OSIGRB_LIBRARY
  NAMES OsiGrb libOsiGrb
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OsiGrb REQUIRED_VARS COIN_OSIGRB_LIBRARY)

mark_as_advanced(COIN_OSIGRB_LIBRARY)
