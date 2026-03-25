#[=======================================================================[.rst:
gtopt_solver_cplex
------------------

MODULE library implementing the SolverBackend interface via the CPLEX
C Callable Library (``libcplex``).  Uses the CPLEX C API directly
(no OSI wrapper) for full access to CPLEX features.

#]=======================================================================]

# ---- Sources ----

set(_cplex_sources
    cplex_solver_backend.cpp
    cplex_plugin.cpp
)

# ---- Module library ----

add_library(gtopt_solver_cplex MODULE ${_cplex_sources})

# ---- Include paths ----

target_include_directories(gtopt_solver_cplex
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(gtopt_solver_cplex SYSTEM
  PRIVATE
    "${CPLEX_INCLUDE_DIRS}"
)

# ---- Link dependencies ----

target_link_libraries(gtopt_solver_cplex
  PRIVATE
    gtopt::gtopt
    ${CPLEX_LIBRARIES}
)

# ---- Output location ----

set_target_properties(gtopt_solver_cplex PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
  PREFIX "lib"
  INSTALL_RPATH_USE_LINK_PATH TRUE
)

# ---- RPATH for installed plugin ----
# Preserve RPATH to CPLEX libraries so that dlopen() can find them
# after installation.

include(InstallRpath)
configure_install_rpath(gtopt_solver_cplex)

# ---- Install ----

install(
  TARGETS gtopt_solver_cplex
  LIBRARY DESTINATION lib/gtopt/plugins
)
