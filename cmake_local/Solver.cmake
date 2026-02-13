include_guard(GLOBAL)

find_package(COIN)

if(COIN_USE_CPX)
  find_package(CPLEX)
  if(CPLEX_FOUND)
    list(APPEND SOLVER_LIBRARIES ${CPLEX_LIBRARIES})

    find_package(OsiCpx)
    if(OSICPX_FOUND)
      list(INSERT COIN_OSI_LIBRARIES 0 ${COIN_OSICPX_LIBRARY})
    endif()

    add_compile_definitions(COIN_USE_CPX)
  endif()
endif()

if(COIN_USE_CLP)
  find_package(Clp)
  if(CLP_FOUND)
    list(APPEND SOLVER_LIBRARIES ${COIN_CLP_LIBRARIES})

    find_package(OsiClp)
    if(OSICLP_FOUND)
      list(INSERT COIN_OSI_LIBRARIES 0 ${COIN_OSICLP_LIBRARY})
    endif()

    add_compile_definitions(COIN_USE_CLP)
  endif()
endif()

if(COIN_USE_CBC)
  find_package(Cbc)
  if(CBC_FOUND)
    list(APPEND SOLVER_LIBRARIES ${COIN_CBC_LIBRARIES})

    find_package(OsiCbc)
    if(OSICBC_FOUND)
      list(INSERT COIN_OSI_LIBRARIES 0 ${COIN_OSICBC_LIBRARY})
    endif()

    add_compile_definitions(COIN_USE_CBC)
  endif()
endif()
