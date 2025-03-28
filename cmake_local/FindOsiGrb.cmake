find_library(
  COIN_OSIGRB_LIBRARY
  NAMES OsiGrb libOsiGrb
  HINTS ${COIN_ROOT_DIR}/lib/coin
  HINTS ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OSIGRB DEFAULT_MSG COIN_OSIGRB_LIBRARY)
