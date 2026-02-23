#[=======================================================================[.rst:
FindCOIN
--------

Find the COIN-OR Osi (Open Solver Interface) and CoinUtils libraries.

Imported variables
^^^^^^^^^^^^^^^^^^

``COIN_FOUND``
  True if COIN-OR Osi and CoinUtils were found.

``COIN_INCLUDE_DIRS``
  Include directories for COIN-OR headers.

``COIN_OSI_LIBRARIES``
  Libraries to link against (Osi, CoinUtils, LAPACK, ZLIB, Threads, LibM).

``COIN_LIBRARIES``
  Alias for ``COIN_OSI_LIBRARIES``.

Cache variables
^^^^^^^^^^^^^^^

``COIN_ROOT_DIR``
  Hint for the COIN-OR installation prefix.

#]=======================================================================]

include_guard(GLOBAL)

find_path(
  COIN_INCLUDE_DIR coin/CoinUtilsConfig.h
  HINTS ${COIN_ROOT_DIR}/include
)

find_library(
  COIN_COIN_UTILS_LIBRARY
  NAMES CoinUtils libCoinUtils
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

find_library(
  COIN_OSI_LIBRARY
  NAMES Osi libOsi
  HINTS ${COIN_ROOT_DIR}/lib/coin ${COIN_ROOT_DIR}/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  COIN
  REQUIRED_VARS COIN_INCLUDE_DIR COIN_OSI_LIBRARY COIN_COIN_UTILS_LIBRARY
)

if(COIN_FOUND)
  set(COIN_INCLUDE_DIRS "${COIN_INCLUDE_DIR}")
  set(COIN_OSI_LIBRARIES "${COIN_OSI_LIBRARY};${COIN_COIN_UTILS_LIBRARY}")
  set(COIN_LIBRARIES "${COIN_OSI_LIBRARIES}")

  # Save and restore CMAKE_CXX_STANDARD around find_package(LAPACK) because its
  # internal try_compile inherits the global standard, and CXX26 may not yet be
  # supported by the compiler for simple C-linkage checks. C++17 is used as it
  # is widely supported by all modern compilers.
  set(_COIN_SAVED_CXX_STANDARD ${CMAKE_CXX_STANDARD})
  set(CMAKE_CXX_STANDARD 17)
  find_package(LAPACK QUIET)
  set(CMAKE_CXX_STANDARD ${_COIN_SAVED_CXX_STANDARD})
  unset(_COIN_SAVED_CXX_STANDARD)

  if(NOT LAPACK_FOUND)
    # CMake's FindLAPACK requires BLAS to be found first; if BLAS is absent
    # (e.g. libblas-dev not installed) the whole search fails even when
    # liblapack-dev is present.  Fall back to a direct find_library search so
    # that libCoinUtils.so can still be linked against the LAPACK routines it
    # needs (dgetrf_, dgetrs_).
    find_library(_COIN_LAPACK_LIB NAMES lapack)
    find_library(_COIN_BLAS_LIB NAMES blas openblas)
    if(_COIN_LAPACK_LIB)
      set(LAPACK_LIBRARIES "${_COIN_LAPACK_LIB}")
      if(_COIN_BLAS_LIB)
        list(APPEND LAPACK_LIBRARIES "${_COIN_BLAS_LIB}")
      endif()
      message(STATUS "COIN: FindLAPACK fallback found ${_COIN_LAPACK_LIB}")
    else()
      message(WARNING "COIN: LAPACK not found; libCoinUtils.so may fail to link")
    endif()
  endif()

  find_package(ZLIB)
  find_package(Threads)
  find_package(LibM)

  set(COIN_OSI_LIBRARIES
      "${COIN_OSI_LIBRARIES};${LAPACK_LIBRARIES};${ZLIB_LIBRARIES};${CMAKE_THREAD_LIBS_INIT};${LibM_LIBRARIES}"
  )

  message(STATUS "COIN OSi Libs: ${COIN_OSI_LIBRARIES}")
endif()

mark_as_advanced(COIN_INCLUDE_DIR COIN_COIN_UTILS_LIBRARY COIN_OSI_LIBRARY
                 _COIN_LAPACK_LIB _COIN_BLAS_LIB)
