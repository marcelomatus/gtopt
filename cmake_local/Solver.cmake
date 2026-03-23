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
set(COIN_ROOT_DIR
    "/opt/coinor"
    CACHE PATH "COIN-OR installation prefix"
)
set(CPLEX_ROOT_DIR
    "/opt/cplex"
    CACHE PATH "IBM ILOG CPLEX installation prefix"
)

find_package(COIN)

# ---- Solver selection ----
#
# COIN_SOLVER chooses which LP/MIP back-end to use through the COIN-OR Osi
# layer.  Accepted values:
#
#   AUTO  – probe for installed solvers in order CPX → HGS → CLP → CBC and
#           pick the first one found (default).
#   CLP   – use Clp  (COIN-OR LP solver).
#   CBC   – use Cbc  (COIN-OR MIP solver, implies Clp).
#   CPX   – use IBM ILOG CPLEX.
#   HGS   – use HiGHS (MIT-licensed LP/MIP solver, requires OsiHiGHS).
#
set(COIN_SOLVER "AUTO" CACHE STRING "COIN-OR solver back-end (AUTO, CLP, CBC, CPX, HGS)")
set_property(CACHE COIN_SOLVER PROPERTY STRINGS AUTO CLP CBC CPX HGS)

string(TOUPPER "${COIN_SOLVER}" _COIN_SOLVER_UPPER)

# --- helper: try to locate a solver and record success in _<NAME>_AVAILABLE --

macro(_coin_probe_cpx)
  find_package(CPLEX QUIET)
  set(_CPX_AVAILABLE ${CPLEX_FOUND})
endmacro()

macro(_coin_probe_cbc)
  find_package(Cbc QUIET)
  set(_CBC_AVAILABLE ${CBC_FOUND})
endmacro()

macro(_coin_probe_clp)
  find_package(Clp QUIET)
  set(_CLP_AVAILABLE ${CLP_FOUND})
endmacro()

macro(_coin_probe_hgs)
  find_package(HiGHS QUIET)
  set(_HGS_AVAILABLE ${HIGHS_FOUND})
endmacro()

# --- AUTO: probe in priority order and pick the first available solver --------

if(_COIN_SOLVER_UPPER STREQUAL "AUTO")
  _coin_probe_cpx()
  if(_CPX_AVAILABLE)
    set(_COIN_SOLVER_UPPER "CPX")
    message(STATUS "COIN_SOLVER=AUTO: detected CPLEX")
  else()
    _coin_probe_hgs()
    if(_HGS_AVAILABLE)
      set(_COIN_SOLVER_UPPER "HGS")
      message(STATUS "COIN_SOLVER=AUTO: detected HiGHS")
    else()
      _coin_probe_clp()
      if(_CLP_AVAILABLE)
        set(_COIN_SOLVER_UPPER "CLP")
        message(STATUS "COIN_SOLVER=AUTO: detected CLP")
      else()
        _coin_probe_cbc()
        if(_CBC_AVAILABLE)
          set(_COIN_SOLVER_UPPER "CBC")
          message(STATUS "COIN_SOLVER=AUTO: detected CBC")
        else()
          set(_COIN_SOLVER_UPPER "NONE")
          message(STATUS "COIN_SOLVER=AUTO: no solver found")
        endif()
      endif()
    endif()
  endif()
endif()

# --- Configure the selected solver -------------------------------------------

if(_COIN_SOLVER_UPPER STREQUAL "CPX")
  find_package(CPLEX)
  if(CPLEX_FOUND)
    list(APPEND SOLVER_LIBRARIES ${CPLEX_LIBRARIES})

    find_package(OsiCpx)
    if(OSICPX_FOUND)
      list(INSERT COIN_OSI_LIBRARIES 0 ${COIN_OSICPX_LIBRARY})
    endif()

    add_compile_definitions(COIN_USE_CPX)
    message(STATUS "COIN solver: CPLEX")
  endif()

elseif(_COIN_SOLVER_UPPER STREQUAL "CBC")
  find_package(Cbc)
  if(CBC_FOUND)
    list(APPEND SOLVER_LIBRARIES ${COIN_CBC_LIBRARIES})

    find_package(OsiCbc)
    if(OSICBC_FOUND)
      list(INSERT COIN_OSI_LIBRARIES 0 ${COIN_OSICBC_LIBRARY})
    endif()

    add_compile_definitions(COIN_USE_CBC)
    message(STATUS "COIN solver: CBC")
  endif()

elseif(_COIN_SOLVER_UPPER STREQUAL "CLP")
  find_package(Clp)
  if(CLP_FOUND)
    list(APPEND SOLVER_LIBRARIES ${COIN_CLP_LIBRARIES})

    find_package(OsiClp)
    if(OSICLP_FOUND)
      list(INSERT COIN_OSI_LIBRARIES 0 ${COIN_OSICLP_LIBRARY})
    endif()

    add_compile_definitions(COIN_USE_CLP)
    message(STATUS "COIN solver: CLP")
  endif()

elseif(_COIN_SOLVER_UPPER STREQUAL "HGS")
  find_package(HiGHS)
  if(HIGHS_FOUND)
    list(APPEND SOLVER_LIBRARIES ${HIGHS_LIBRARIES})
    list(APPEND SOLVER_INCLUDE_DIRS ${HIGHS_INCLUDE_DIRS})

    find_package(OsiHiGHS QUIET)
    if(OSIHIGHS_FOUND)
      list(INSERT COIN_OSI_LIBRARIES 0 ${COIN_OSIHIGHS_LIBRARY})
    endif()

    add_compile_definitions(COIN_USE_HGS)
    message(STATUS "COIN solver: HiGHS")
  endif()

elseif(_COIN_SOLVER_UPPER STREQUAL "NONE")
  message(STATUS "COIN solver: none configured")

else()
  message(FATAL_ERROR "Unknown COIN_SOLVER value: '${COIN_SOLVER}'. Use AUTO, CLP, CBC, CPX, or HGS.")
endif()
