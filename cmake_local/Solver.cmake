include_guard(GLOBAL)

# ---- Module search path ----
# Append cmake_local/ and cmake/ to the module search path so that the
# Find*.cmake modules (FindCOIN, FindClp, FindCbc, …) located in cmake_local/
# are discoverable by find_package().  Using list(APPEND …) preserves any
# paths already set by the caller; list(REMOVE_DUPLICATES …) keeps the list
# clean on repeated includes across super-project builds.
#
# CMAKE_CURRENT_LIST_DIR is cmake_local/ (the directory containing this file).
get_filename_component(_solver_repo_root "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}"    # cmake_local/  – Find*.cmake modules live here
     "${_solver_repo_root}/cmake"   # cmake/        – CPM, tools, etc.
)
list(REMOVE_DUPLICATES CMAKE_MODULE_PATH)
unset(_solver_repo_root)

# ---- COIN-OR and solver root hints ----
# These cache variables let users point CMake at non-standard COIN-OR or
# CPLEX installations.  Defining them here (rather than in every consumer
# CMakeLists.txt) keeps the interface in a single place.
# Default COIN_ROOT_DIR to the first prefix that ships the COMPLETE stack —
# specifically libCbc.  /opt/coinor historically ships only CLP/OSI (no CBC);
# the system prefix (/usr) ships the full CoinUtils+Osi+Clp+Cgl+Cbc+OsiCbc set.
# Pointing COIN_ROOT_DIR at /opt/coinor while CBC is only under /usr produces a
# MIXED stack (CLP from /opt, CBC from /usr) that trips the osi plugin's
# ABI-consistency guard and silently drops CBC, a MIP backend.  Probe the
# candidates for libCbc and default to the first that has it, so CBC is enabled
# wherever the full stack exists.  A user `-DCOIN_ROOT_DIR=` or any cached value
# always wins (the cache var is only defaulted when unset), so this never
# overrides an env that deliberately pins /opt/coinor.
if(NOT COIN_ROOT_DIR)
  set(_coin_default "/opt/coinor")
  foreach(_coin_cand "/opt/coinor" "/usr" "/usr/local")
    file(GLOB _coin_cbc
         "${_coin_cand}/lib/libCbc.*"
         "${_coin_cand}/lib/*/libCbc.*"
         "${_coin_cand}/lib64/libCbc.*")
    if(_coin_cbc)
      set(_coin_default "${_coin_cand}")
      break()
    endif()
  endforeach()
  set(COIN_ROOT_DIR
      "${_coin_default}"
      CACHE PATH "COIN-OR installation prefix"
  )
  unset(_coin_default)
  unset(_coin_cand)
  unset(_coin_cbc)
endif()
set(CPLEX_ROOT_DIR
    "/opt/cplex"
    CACHE PATH "IBM ILOG CPLEX installation prefix"
)

# ---- Detect COIN-OR ----
# Sets COIN_FOUND, COIN_INCLUDE_DIRS, COIN_OSI_LIBRARIES, etc.
# Individual solver backends (CLP, CBC, CPLEX, HiGHS) are discovered and
# configured by each plugin's own CMakeLists.txt (plugins/osi/, plugins/highs/)
# at build time.  Solvers are loaded at runtime via the plugin system, so no
# compile-time solver selection is needed here.
find_package(COIN)
