if(COIN_OSI_LIBRARY)
  message(STATUS "Looking for Coin Osi:")
endif()

if(COIN_INCLUDE_DIR
   AND COIN_COIN_UTILS_LIBRARY
   AND COIN_OSI_LIBRARY
)
  message(STATUS "  Already on Cache ${COIN_OSI_LIBRARY} ${COIN_COIN_UTILS_LIBRARY}")
  # in cache already
  set(COIN_FOUND TRUE)
  set(COIN_INCLUDE_DIRS "${COIN_INCLUDE_DIR}")
  set(COIN_LIBRARIES "${COIN_LIBRARY};${COIN_CXX_LIBRARY}")

  set(COIN_OSI_LIBRARIES "${COIN_OSI_LIBRARY};${COIN_COIN_UTILS_LIBRARY}")
  set(COIN_LIBRARIES "${COIN_OSI_LIBRARIES}")

else(COIN_INCLUDE_DIR)

  find_path(COIN_INCLUDE_DIR coin/CoinUtilsConfig.h HINTS ${COIN_ROOT_DIR}/include)

  find_library(
    COIN_COIN_UTILS_LIBRARY
    NAMES CoinUtils libCoinUtils
    HINTS ${COIN_ROOT_DIR}/lib/coin
    HINTS ${COIN_ROOT_DIR}/lib
  )

  find_library(
    COIN_OSI_LIBRARY
    NAMES Osi libOsi
    HINTS ${COIN_ROOT_DIR}/lib/coin
    HINTS ${COIN_ROOT_DIR}/lib
  )

  if(COIN_OSI_LIBRARY)
    message(STATUS "Coin Osi found: ${COIN_OSI_LIBRARY}")
  endif()

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(
    COIN DEFAULT_MSG COIN_INCLUDE_DIR COIN_OSI_LIBRARY COIN_COIN_UTILS_LIBRARY
  )

  set(COIN_INCLUDE_DIRS "${COIN_INCLUDE_DIR}")
  set(COIN_OSI_LIBRARIES "${COIN_OSI_LIBRARY};${COIN_COIN_UTILS_LIBRARY}")
  set(COIN_LIBRARIES "${COIN_OSI_LIBRARIES}")

endif()

include_directories(SYSTEM "${COIN_INCLUDE_DIR}")

find_package(LAPACK)
find_package(ZLIB)
find_package(Threads)
find_package(LibM)

set(COIN_OSI_LIBRARIES
    "${COIN_OSI_LIBRARIES};${LAPACK_LIBRARIES};${ZLIB_LIBRARIES};${CMAKE_THREAD_LIBS_INIT};${LibM_LIBRARIES}"
)

message(STATUS "COIN OSi Libs: ${COIN_OSI_LIBRARIES}")

# Mark as advanced options in ccmake:
mark_as_advanced(COIN_INCLUDE_DIRS COIN_OSI_LIBRARES COIN_LIBRARIES)
