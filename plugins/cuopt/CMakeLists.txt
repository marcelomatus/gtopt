#[=======================================================================[.rst:
gtopt_solver_cuopt
------------------

MODULE library implementing the SolverBackend interface via the NVIDIA
cuOpt GPU solver's stable C API (``cuopt/mathematical_optimization/cuopt_c.h``,
``libcuopt.so``).  Uses ONLY the C API — no CUDA toolkit headers, no
RAFT/RMM C++ templates — so it builds with the project's normal compiler.

cuOpt has no live-mutation API, so this backend keeps its own mutable
host model and rebuilds the immutable cuOpt problem on every solve
(buffer-and-replay).  See cuopt_solver_backend.hpp for the design.

#]=======================================================================]

include(GtoptPlugin)

gtopt_add_solver_plugin(
  NAME       gtopt_solver_cuopt
  SOURCES    cuopt_solver_backend.cpp cuopt_plugin.cpp
  INCLUDES   "${CUOPT_INCLUDE_DIRS}"
  LIBRARIES  ${CUOPT_LIBRARIES}
)
