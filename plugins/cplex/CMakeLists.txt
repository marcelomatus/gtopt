#[=======================================================================[.rst:
gtopt_solver_cplex
------------------

MODULE library implementing the SolverBackend interface via the CPLEX
C Callable Library (``libcplex``).  Uses the CPLEX C API directly
(no OSI wrapper) for full access to CPLEX features.

#]=======================================================================]

include(GtoptPlugin)

gtopt_add_solver_plugin(
  NAME       gtopt_solver_cplex
  SOURCES    cplex_solver_backend.cpp cplex_plugin.cpp
  INCLUDES   "${CPLEX_INCLUDE_DIRS}"
  LIBRARIES  ${CPLEX_LIBRARIES}
)
