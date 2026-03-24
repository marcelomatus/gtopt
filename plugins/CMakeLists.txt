#[=======================================================================[.rst:
Plugins
-------

Build solver backend plugins for gtopt.  Each plugin is a MODULE library
(dlopen-loaded at runtime) that implements the SolverBackend interface.

Plugins are discovered at runtime by SolverRegistry, which scans for
``libgtopt_solver_*.so`` files in standard paths.

#]=======================================================================]

# ---- OSI plugin (CLP / CBC / CPLEX) ----
# Always attempt to build if COIN-OR is available (it was found by the
# parent project's Solver.cmake).

if(COIN_FOUND)
  add_subdirectory(osi)
else()
  message(STATUS "Solver plugin: OSI skipped (COIN-OR not found)")
endif()

# ---- HiGHS plugin ----

find_package(HiGHS QUIET)
if(HIGHS_FOUND)
  add_subdirectory(highs)
else()
  message(STATUS "Solver plugin: HiGHS skipped (HiGHS not found)")
endif()
