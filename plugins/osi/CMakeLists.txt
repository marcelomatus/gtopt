#[=======================================================================[.rst:
gtopt_solver_osi
----------------

MODULE library implementing the SolverBackend interface via COIN-OR OSI.
Provides CLP and CBC solver backends depending on available
COIN-OR packages.

#]=======================================================================]

# ---- Sources ----

set(_osi_sources
    osi_solver_backend.cpp
    osi_plugin.cpp
)

# ---- Module library ----

add_library(gtopt_solver_osi MODULE ${_osi_sources})

# ---- Include paths ----
# Plugin headers (private) + gtopt public headers + COIN-OR headers

target_include_directories(gtopt_solver_osi
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(gtopt_solver_osi SYSTEM
  PRIVATE
    "${COIN_INCLUDE_DIRS}"
)

# ---- Link dependencies ----
# Link gtopt (for SolverBackend, SolverOptions headers) and COIN-OR libs.

target_link_libraries(gtopt_solver_osi
  PRIVATE
    gtopt::gtopt
    "${COIN_OSI_LIBRARIES}"
)

# ---- Solver-specific compile definitions and libraries ----

# CLP is always available when COIN_FOUND
find_package(OsiClp QUIET)
if(OSICLP_FOUND)
  find_package(Clp QUIET)

  # ---- ABI consistency check ----
  # Verify CLP/OsiClp come from the same installation as core Osi/CoinUtils.
  set(_coin_clp_ok TRUE)
  if(COIN_LIBRARY_DIR AND COIN_OSICLP_LIBRARY)
    get_filename_component(_osiclp_dir "${COIN_OSICLP_LIBRARY}" DIRECTORY)
    if(NOT _osiclp_dir STREQUAL "${COIN_LIBRARY_DIR}")
      message(WARNING
        "COIN-OR ABI mismatch: OsiClp found at ${COIN_OSICLP_LIBRARY} "
        "but core Osi/CoinUtils are at ${COIN_LIBRARY_DIR}. "
        "Skipping CLP support. To fix: install all COIN-OR packages from "
        "the same source."
      )
      set(_coin_clp_ok FALSE)
    endif()
  endif()

  if(_coin_clp_ok)
    target_link_libraries(gtopt_solver_osi PRIVATE
      ${COIN_OSICLP_LIBRARY} ${COIN_CLP_LIBRARIES}
    )
    target_compile_definitions(gtopt_solver_osi PRIVATE COIN_USE_CLP)
  else()
    message(STATUS "Solver plugin: CLP disabled due to COIN-OR ABI mismatch")
  endif()
endif()

# CBC
find_package(OsiCbc QUIET)
if(OSICBC_FOUND)
  find_package(Cbc QUIET)

  # ---- ABI consistency check ----
  # Verify that CBC/OsiCbc libraries come from the same COIN-OR installation
  # as the core Osi/CoinUtils libraries.  Mixing libraries from different
  # builds (e.g. CLP from /opt/coinor + CBC from system /usr/lib) causes
  # ABI-incompatible undefined-symbol errors at runtime such as:
  #   undefined symbol: _ZTv0_n24_N21OsiClpSolverInterface12initialSolveEv
  # See: https://github.com/marcelomatus/gtopt/issues/302
  set(_coin_cbc_ok TRUE)
  if(COIN_LIBRARY_DIR AND COIN_OSICBC_LIBRARY)
    get_filename_component(_osicbc_dir "${COIN_OSICBC_LIBRARY}" DIRECTORY)
    if(NOT _osicbc_dir STREQUAL "${COIN_LIBRARY_DIR}")
      message(WARNING
        "COIN-OR ABI mismatch: OsiCbc found at ${COIN_OSICBC_LIBRARY} "
        "but core Osi/CoinUtils are at ${COIN_LIBRARY_DIR}. "
        "Mixing COIN-OR libraries from different installations causes "
        "undefined-symbol errors at runtime. "
        "Skipping CBC support. To fix: install all COIN-OR packages from "
        "the same source (either all from system packages, or all from "
        "a custom build with -DCOIN_ROOT_DIR=/path/to/coinor)."
      )
      set(_coin_cbc_ok FALSE)
    endif()
  endif()
  if(COIN_LIBRARY_DIR AND COIN_CBC_LIBRARY AND _coin_cbc_ok)
    get_filename_component(_cbc_dir "${COIN_CBC_LIBRARY}" DIRECTORY)
    if(NOT _cbc_dir STREQUAL "${COIN_LIBRARY_DIR}")
      message(WARNING
        "COIN-OR ABI mismatch: Cbc found at ${COIN_CBC_LIBRARY} "
        "but core Osi/CoinUtils are at ${COIN_LIBRARY_DIR}. "
        "Mixing COIN-OR libraries from different installations causes "
        "undefined-symbol errors at runtime. "
        "Skipping CBC support. To fix: install all COIN-OR packages from "
        "the same source (either all from system packages, or all from "
        "a custom build with -DCOIN_ROOT_DIR=/path/to/coinor)."
      )
      set(_coin_cbc_ok FALSE)
    endif()
  endif()

  if(_coin_cbc_ok)
    target_link_libraries(gtopt_solver_osi PRIVATE
      ${COIN_OSICBC_LIBRARY} ${COIN_CBC_LIBRARIES}
    )
    target_compile_definitions(gtopt_solver_osi PRIVATE GTOPT_OSI_HAS_CBC)
  else()
    message(STATUS "Solver plugin: CBC disabled due to COIN-OR ABI mismatch")
  endif()
endif()

# ---- Output location ----
# Place the plugin next to the standalone binary for easy discovery.

set_target_properties(gtopt_solver_osi PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
  PREFIX "lib"
  INSTALL_RPATH_USE_LINK_PATH TRUE
)

# ---- RPATH for installed plugin ----
# Preserve RPATH to non-system solver libraries (e.g. COIN-OR at /opt/coinor)
# so that dlopen() can find them after installation.

include(InstallRpath)
configure_install_rpath(gtopt_solver_osi)

# ---- Install ----

install(
  TARGETS gtopt_solver_osi
  LIBRARY DESTINATION lib/gtopt/plugins
)
