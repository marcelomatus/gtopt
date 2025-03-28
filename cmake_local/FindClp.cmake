find_library(
  COIN_CLP_LIBRARY
  NAMES Clp libClp
  HINTS ${COIN_ROOT_DIR}/lib/coin
  HINTS ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Clp DEFAULT_MSG COIN_CLP_LIBRARY)

set(COIN_CLP_LIBRARIES "${COIN_CLP_LIBRARY}")
