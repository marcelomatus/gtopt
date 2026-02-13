include_guard(GLOBAL)

find_library(
  COIN_CBC_LIBRARY
  NAMES Cbc libCbc
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Cbc REQUIRED_VARS COIN_CBC_LIBRARY)

if(CBC_FOUND)
  set(COIN_CBC_LIBRARIES "${COIN_CBC_LIBRARY}")
endif()

mark_as_advanced(COIN_CBC_LIBRARY)
