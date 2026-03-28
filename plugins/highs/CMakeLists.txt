#[=======================================================================[.rst:
gtopt_solver_highs
------------------

MODULE library implementing the SolverBackend interface via the native
HiGHS C++ API.  Requires HiGHS >= 1.5.0.

#]=======================================================================]

include(GtoptPlugin)

gtopt_add_solver_plugin(
  NAME       gtopt_solver_highs
  SOURCES    highs_solver_backend.cpp highs_plugin.cpp
  INCLUDES   "${HIGHS_INCLUDE_DIRS}"
  LIBRARIES  ${HIGHS_LIBRARIES}
)
