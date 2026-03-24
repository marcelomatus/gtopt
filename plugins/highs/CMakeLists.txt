#[=======================================================================[.rst:
gtopt_solver_highs
------------------

MODULE library implementing the SolverBackend interface via the native
HiGHS C++ API.  Requires HiGHS >= 1.5.0 (native API without OSI wrapper).

#]=======================================================================]

# ---- Sources ----

set(_highs_sources
    highs_solver_backend.cpp
    highs_plugin.cpp
)

# ---- Module library ----

add_library(gtopt_solver_highs MODULE ${_highs_sources})

# ---- Include paths ----

target_include_directories(gtopt_solver_highs
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(gtopt_solver_highs SYSTEM
  PRIVATE
    "${HIGHS_INCLUDE_DIRS}"
)

# ---- Link dependencies ----

target_link_libraries(gtopt_solver_highs
  PRIVATE
    gtopt::gtopt
    ${HIGHS_LIBRARIES}
)

# ---- Output location ----

set_target_properties(gtopt_solver_highs PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
  PREFIX "lib"
)

# ---- Install ----

install(
  TARGETS gtopt_solver_highs
  LIBRARY DESTINATION lib/gtopt/plugins
)
