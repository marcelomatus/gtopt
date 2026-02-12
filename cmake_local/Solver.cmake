find_package(COIN)

if(COIN_USE_CPX)
  find_package(CPLEX)
  if(CPLEX_FOUND)
    set(SOLVER_LIBRARIES "${SOLVER_LIBRARIES};${CPLEX_LIBRARIES}")

    find_package(OsiCpx)
    if(OSICPX_FOUND)
      set(COIN_OSI_LIBRARIES "${COIN_OSICPX_LIBRARY};${COIN_OSI_LIBRARIES}")
    endif()

    add_compile_definitions(COIN_USE_CPX)
  endif()
endif()

if(COIN_USE_CLP)
  find_package(Clp)
  if(CLP_FOUND)
    set(SOLVER_LIBRARIES "${SOLVER_LIBRARIES};${COIN_CLP_LIBRARIES}")

    find_package(OsiClp)
    if(OSICLP_FOUND)
      set(COIN_OSI_LIBRARIES "${COIN_OSICLP_LIBRARY};${COIN_OSI_LIBRARIES}")
    endif()

    add_compile_definitions(COIN_USE_CLP)
  endif()
endif()

if(COIN_USE_CBC)
  find_package(Cbc)
  if(CBC_FOUND)
    set(SOLVER_LIBRARIES "${SOLVER_LIBRARIES};${COIN_CBC_LIBRARIES}")

    find_package(OsiCbc)
    if(OSICBC_FOUND)
      set(COIN_OSI_LIBRARIES "${COIN_OSICBC_LIBRARY};${COIN_OSI_LIBRARIES}")
    endif()

    add_compile_definitions(COIN_USE_CBC)
  endif()
endif()
