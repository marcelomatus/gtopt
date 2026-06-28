#[=======================================================================[.rst:
gtopt_solver_scip
-----------------

MODULE library implementing the SolverBackend interface via the SCIP
constraint-integer-programming / MIP solver's C API (``scip/scip.h``,
``libscip.so``).  Requires SCIP >= 9 (Apache-2.0).

SCIP exposes a staged build/solve C API and no live-mutation path, so this
backend keeps its own mutable host model and rebuilds the SCIP problem on
every solve (buffer-and-replay).  See scip_solver_backend.hpp for the design.

``SCIP_LIBRARIES`` / ``SCIP_INCLUDE_DIRS`` are provided either by SCIP's own
CMake config (the ``libscip`` imported target) or by the bundled
FindSCIP.cmake fallback (see plugins/CMakeLists.txt).

#]=======================================================================]

include(GtoptPlugin)

gtopt_add_solver_plugin(
  NAME       gtopt_solver_scip
  SOURCES    scip_solver_backend.cpp scip_plugin.cpp
  INCLUDES   "${SCIP_INCLUDE_DIRS}"
  LIBRARIES  ${SCIP_LIBRARIES}
)
