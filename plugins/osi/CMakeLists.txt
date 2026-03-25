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
  target_link_libraries(gtopt_solver_osi PRIVATE
    ${COIN_OSICLP_LIBRARY} ${COIN_CLP_LIBRARIES}
  )
  target_compile_definitions(gtopt_solver_osi PRIVATE COIN_USE_CLP)
endif()

# CBC
find_package(OsiCbc QUIET)
if(OSICBC_FOUND)
  find_package(Cbc QUIET)
  target_link_libraries(gtopt_solver_osi PRIVATE
    ${COIN_OSICBC_LIBRARY} ${COIN_CBC_LIBRARIES}
  )
  target_compile_definitions(gtopt_solver_osi PRIVATE GTOPT_OSI_HAS_CBC)
endif()

# ---- Output location ----
# Place the plugin next to the standalone binary for easy discovery.

set_target_properties(gtopt_solver_osi PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
  PREFIX "lib"
)

# ---- Install ----

install(
  TARGETS gtopt_solver_osi
  LIBRARY DESTINATION lib/gtopt/plugins
)
