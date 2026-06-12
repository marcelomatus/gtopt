#[=======================================================================[.rst:
Plugins
-------

Build solver backend plugins for gtopt.  Each plugin is a MODULE library
(dlopen-loaded at runtime) that implements the SolverBackend interface.

Plugins are discovered at runtime by SolverRegistry, which scans for
``libgtopt_solver_*.so`` files in standard paths:

  1. ``$GTOPT_PLUGIN_DIR``
  2. ``<exe>/../lib/gtopt/plugins/``
  3. ``<exe>/plugins/``
  4. ``/usr/local/lib/gtopt/plugins/``

#]=======================================================================]

# ---- OSI plugin (CLP / CBC) ----
# Always attempt to build if COIN-OR is available (it was found by the
# parent project's Solver.cmake).

if(COIN_FOUND)
  message(STATUS "Solver plugin: OSI enabled (COIN-OR at ${COIN_ROOT_DIR})")
  add_subdirectory(osi)
else()
  message(STATUS "Solver plugin: OSI skipped (COIN-OR not found)")
endif()

# ---- CPLEX plugin (direct C Callable Library, no OSI) ----

find_package(CPLEX QUIET)
if(CPLEX_FOUND)
  message(STATUS "Solver plugin: CPLEX enabled (${CPLEX_VERSION} at ${CPLEX_INCLUDE_DIR})")
  add_subdirectory(cplex)
else()
  message(STATUS "Solver plugin: CPLEX skipped (CPLEX not found; set CPLEX_ROOT_DIR)")
endif()

# ---- HiGHS plugin ----
# HiGHS cmake config installs as lowercase "highs"; try both names.
# Also try pkg-config as a fallback.

find_package(highs QUIET)
if(NOT highs_FOUND)
  find_package(HiGHS QUIET)
endif()
if(NOT highs_FOUND AND NOT HIGHS_FOUND)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(HIGHS QUIET IMPORTED_TARGET "highs >= 1.5")
    if(HIGHS_FOUND AND TARGET PkgConfig::HIGHS)
      set(HIGHS_LIBRARIES PkgConfig::HIGHS)
    endif()
  endif()
endif()

# ---- MindOpt plugin (Alibaba DAMO Academy) ----

find_package(MindOpt QUIET)
if(MINDOPT_FOUND)
  message(STATUS "Solver plugin: MindOpt enabled (${MINDOPT_INCLUDE_DIR})")
  add_subdirectory(mindopt)
else()
  message(STATUS "Solver plugin: MindOpt skipped (MindOpt not found; set MINDOPT_HOME or MINDOPT_ROOT_DIR)")
endif()

# ---- Gurobi plugin ----

find_package(Gurobi QUIET)
if(GUROBI_FOUND)
  message(STATUS "Solver plugin: Gurobi enabled (${GUROBI_INCLUDE_DIR})")
  add_subdirectory(gurobi)
else()
  message(STATUS "Solver plugin: Gurobi skipped (Gurobi not found; set GUROBI_HOME or GUROBI_ROOT_DIR)")
endif()

if(highs_FOUND OR HIGHS_FOUND)
  # Config-mode sets the highs::highs imported target.
  # Legacy Find-module mode sets HIGHS_INCLUDE_DIRS / HIGHS_LIBRARIES.
  if(TARGET highs::highs AND NOT HIGHS_LIBRARIES)
    get_target_property(_highs_inc highs::highs INTERFACE_INCLUDE_DIRECTORIES)
    set(HIGHS_INCLUDE_DIRS "${_highs_inc}" CACHE STRING "HiGHS include dirs")
    set(HIGHS_LIBRARIES highs::highs CACHE STRING "HiGHS libraries")
    set(HIGHS_FOUND TRUE)
  endif()
  if(HIGHS_VERSION)
    message(STATUS "Solver plugin: HiGHS enabled (${HIGHS_VERSION})")
  else()
    message(STATUS "Solver plugin: HiGHS enabled")
  endif()
  add_subdirectory(highs)
else()
  message(STATUS "Solver plugin: HiGHS skipped (HiGHS not found; set CMAKE_PREFIX_PATH)")
endif()
