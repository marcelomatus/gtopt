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

# ---- cuOpt plugin (NVIDIA GPU solver; stable C API only) ----
# Gated like the other optional solvers.  FindCuOpt auto-detects the pip
# `libcuopt-cu12` wheel via `python -c "import libcuopt"`, or honours
# CUOPT_HOME / CUOPT_ROOT_DIR.  Disable with -DGTOPT_DISABLE_CUOPT=ON.
#
# Minimum 26.8: MIP starts coexist with presolve from 26.08 (older
# releases hard-reject cuOptAddMIPStart under presolve), and the 26.08
# C API is the one the backend is validated against.  Until 26.08 goes
# stable, install the nightly:
#   pip install --user --pre --upgrade \
#     --extra-index-url https://pypi.anaconda.org/rapidsai-wheels-nightly/simple \
#     "libcuopt-cu12>=26.8.0a0"

find_package(CuOpt 26.8 QUIET)
if(CUOPT_FOUND)
  if(CUOPT_VERSION)
    message(STATUS "Solver plugin: cuOpt enabled (${CUOPT_VERSION_FULL} at ${CUOPT_INCLUDE_DIR})")
  else()
    message(STATUS "Solver plugin: cuOpt enabled (${CUOPT_INCLUDE_DIR})")
  endif()
  add_subdirectory(cuopt)
elseif(CUOPT_VERSION)
  message(STATUS "Solver plugin: cuOpt skipped (found ${CUOPT_VERSION_FULL}, but >= 26.8 is required; upgrade libcuopt-cu12)")
else()
  message(STATUS "Solver plugin: cuOpt skipped (cuOpt not found; pip install libcuopt-cu12 or set CUOPT_HOME)")
endif()

# ---- SCIP plugin (open-source CIP/MIP solver; Apache-2.0 since v9) ----
# Prefer SCIP's own CMake config (the `libscip` imported target); fall back to
# the bundled FindSCIP.cmake module when the config is unavailable or fails
# (its find_dependency(GMP/MPFR) needs dev headers we don't require to link
# libscip.so).  Disable explicitly with -DGTOPT_DISABLE_SCIP=ON.

if(NOT GTOPT_DISABLE_SCIP)
  find_package(SCIP CONFIG QUIET)
  if(SCIP_FOUND AND NOT SCIP_LIBRARIES AND TARGET libscip)
    set(SCIP_LIBRARIES libscip)
  endif()
  if(NOT SCIP_FOUND)
    find_package(SCIP MODULE QUIET)
  endif()
endif()
if(SCIP_FOUND)
  if(SCIP_VERSION)
    message(STATUS "Solver plugin: SCIP enabled (${SCIP_VERSION})")
  else()
    message(STATUS "Solver plugin: SCIP enabled")
  endif()
  add_subdirectory(scip)
else()
  message(STATUS "Solver plugin: SCIP skipped (SCIP not found; set SCIP_ROOT_DIR or -DGTOPT_DISABLE_SCIP=ON to silence)")
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
