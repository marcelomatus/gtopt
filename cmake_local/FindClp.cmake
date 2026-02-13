include_guard(GLOBAL)

find_library(
  COIN_CLP_LIBRARY
  NAMES Clp libClp
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Clp REQUIRED_VARS COIN_CLP_LIBRARY)

if(CLP_FOUND)
  set(COIN_CLP_LIBRARIES "${COIN_CLP_LIBRARY}")
endif()

mark_as_advanced(COIN_CLP_LIBRARY)
