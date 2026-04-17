#[=======================================================================[.rst:
gtopt_solver_gurobi
-------------------

MODULE library implementing the SolverBackend interface via the Gurobi
C API (``libgurobi<ver>``).  Uses the Gurobi C API directly for full
access to Gurobi LP/MIP features.

#]=======================================================================]

include(GtoptPlugin)

gtopt_add_solver_plugin(
  NAME       gtopt_solver_gurobi
  SOURCES    gurobi_solver_backend.cpp gurobi_plugin.cpp
  INCLUDES   "${GUROBI_INCLUDE_DIRS}"
  LIBRARIES  ${GUROBI_LIBRARIES}
)
