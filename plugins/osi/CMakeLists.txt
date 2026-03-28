#[=======================================================================[.rst:
gtopt_solver_osi
----------------

MODULE library implementing the SolverBackend interface via COIN-OR OSI.
Provides CLP and CBC solver backends depending on available
COIN-OR packages.

#]=======================================================================]

include(GtoptPlugin)

# ---- Base plugin (Osi + CoinUtils) ----

gtopt_add_solver_plugin(
  NAME       gtopt_solver_osi
  SOURCES    osi_solver_backend.cpp osi_plugin.cpp
  INCLUDES   "${COIN_INCLUDE_DIRS}"
  LIBRARIES  "${COIN_OSI_LIBRARIES}"
)

# ---- ABI reference directory (real path, follows symlinks) ----

if(COIN_LIBRARY_DIR)
  get_filename_component(_coin_real_lib_dir "${COIN_LIBRARY_DIR}" REALPATH)
else()
  set(_coin_real_lib_dir "")
endif()

# ---- CLP support ----

find_package(OsiClp QUIET)
if(OSICLP_FOUND)
  find_package(Clp QUIET)

  gtopt_check_coin_abi(
    REFERENCE_DIR "${_coin_real_lib_dir}"
    LIBRARY       "${COIN_OSICLP_LIBRARY}"
    LABEL         "OsiClp"
    RESULT        _clp_abi_ok
  )

  if(_clp_abi_ok)
    target_link_libraries(gtopt_solver_osi PRIVATE
      ${COIN_OSICLP_LIBRARY} ${COIN_CLP_LIBRARIES}
    )
    target_compile_definitions(gtopt_solver_osi PRIVATE COIN_USE_CLP)
  else()
    message(STATUS "Solver plugin: CLP disabled (ABI mismatch)")
  endif()
endif()

# ---- CBC support ----

find_package(OsiCbc QUIET)
if(OSICBC_FOUND)
  find_package(Cbc QUIET)

  # Check both OsiCbc and Cbc libraries for ABI consistency.
  gtopt_check_coin_abi(
    REFERENCE_DIR "${_coin_real_lib_dir}"
    LIBRARY       "${COIN_OSICBC_LIBRARY}"
    LABEL         "OsiCbc"
    RESULT        _cbc_abi_ok
  )
  if(_cbc_abi_ok AND COIN_CBC_LIBRARY)
    gtopt_check_coin_abi(
      REFERENCE_DIR "${_coin_real_lib_dir}"
      LIBRARY       "${COIN_CBC_LIBRARY}"
      LABEL         "Cbc"
      RESULT        _cbc_abi_ok
    )
  endif()

  if(_cbc_abi_ok)
    target_link_libraries(gtopt_solver_osi PRIVATE
      ${COIN_OSICBC_LIBRARY} ${COIN_CBC_LIBRARIES}
    )
    target_compile_definitions(gtopt_solver_osi PRIVATE GTOPT_OSI_HAS_CBC)
  else()
    message(STATUS "Solver plugin: CBC disabled (ABI mismatch)")
  endif()
endif()
