include_guard(GLOBAL)

find_package(COIN)

# ---- Solver selection ----
#
# COIN_SOLVER chooses which LP/MIP back-end to use through the COIN-OR Osi
# layer.  Accepted values:
#
#   AUTO  – probe for installed solvers in order CPX → CBC → CLP and pick the
#           first one found (default).
#   CLP   – use Clp  (COIN-OR LP solver).
#   CBC   – use Cbc  (COIN-OR MIP solver, implies Clp).
#   CPX   – use IBM ILOG CPLEX.
#
set(COIN_SOLVER "AUTO" CACHE STRING "COIN-OR solver back-end (AUTO, CLP, CBC, CPX)")
set_property(CACHE COIN_SOLVER PROPERTY STRINGS AUTO CLP CBC CPX)

string(TOUPPER "${COIN_SOLVER}" _COIN_SOLVER_UPPER)

# --- helper: try to locate a solver and record success in _<NAME>_AVAILABLE --

macro(_coin_probe_cpx)
  find_package(CPLEX QUIET)
  if(CPLEX_FOUND)
    find_package(OsiCpx QUIET)
  endif()
  set(_CPX_AVAILABLE ${CPLEX_FOUND})
endmacro()

macro(_coin_probe_cbc)
  find_package(Cbc QUIET)
  if(CBC_FOUND)
    find_package(OsiCbc QUIET)
  endif()
  set(_CBC_AVAILABLE ${CBC_FOUND})
endmacro()

macro(_coin_probe_clp)
  find_package(Clp QUIET)
  if(CLP_FOUND)
    find_package(OsiClp QUIET)
  endif()
  set(_CLP_AVAILABLE ${CLP_FOUND})
endmacro()

# --- AUTO: probe in priority order and pick the first available solver --------

if(_COIN_SOLVER_UPPER STREQUAL "AUTO")
  _coin_probe_cpx()
  if(_CPX_AVAILABLE)
    set(_COIN_SOLVER_UPPER "CPX")
    message(STATUS "COIN_SOLVER=AUTO: detected CPLEX")
  else()
    _coin_probe_cbc()
    if(_CBC_AVAILABLE)
      set(_COIN_SOLVER_UPPER "CBC")
      message(STATUS "COIN_SOLVER=AUTO: detected CBC")
    else()
      _coin_probe_clp()
      if(_CLP_AVAILABLE)
        set(_COIN_SOLVER_UPPER "CLP")
        message(STATUS "COIN_SOLVER=AUTO: detected CLP")
      else()
        set(_COIN_SOLVER_UPPER "NONE")
        message(STATUS "COIN_SOLVER=AUTO: no solver found")
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

elseif(_COIN_SOLVER_UPPER STREQUAL "NONE")
  message(STATUS "COIN solver: none configured")

else()
  message(FATAL_ERROR "Unknown COIN_SOLVER value: '${COIN_SOLVER}'. Use AUTO, CLP, CBC, or CPX.")
endif()
