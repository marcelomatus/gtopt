find_library(
  COIN_CBC_LIBRARY
  NAMES Cbc libCbc
  HINTS ${COIN_ROOT_DIR}/lib/coin
  HINTS ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Cbc DEFAULT_MSG COIN_CBC_LIBRARY)

set(COIN_CBC_LIBRARIES "${COIN_CBC_LIBRARY}")
