#[=======================================================================[.rst:
gtopt_solver_mindopt
--------------------

MODULE library implementing the SolverBackend interface via the MindOpt
C API (``libmindopt``).  Uses the MindOpt C API directly for full access
to MindOpt LP/MIP features.

#]=======================================================================]

include(GtoptPlugin)

gtopt_add_solver_plugin(
  NAME       gtopt_solver_mindopt
  SOURCES    mindopt_solver_backend.cpp mindopt_plugin.cpp
  INCLUDES   "${MINDOPT_INCLUDE_DIRS}"
  LIBRARIES  ${MINDOPT_LIBRARIES}
)
