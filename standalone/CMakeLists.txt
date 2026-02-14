cmake_minimum_required(VERSION 3.31.6)

project(gtoptStandalone LANGUAGES CXX)

# ---- Options ----

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)
option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first" ON)
option(USE_CCACHE "use ccache" YES)

cmake_policy(SET CMP0167 NEW)
set(CPPCHECK_ARGS
    "--check-level=exhaustive;--suppress=returnDanglingLifetime;--suppress=internalAstError"
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

CPMAddPackage("gh:TheLartians/Format.cmake@1.8.3")

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../cmake_local
                      ${CMAKE_CURRENT_LIST_DIR}/../cmake
)

set(COIN_ROOT_DIR
    "/opt/coinor"
    CACHE PATH "Coinor root directory"
)
set(CPLEX_ROOT_DIR
    "/opt/cplex"
    CACHE PATH "CPLEX root directory"
)
# Solver back-end: AUTO (default), CLP, CBC, or CPX (CPLEX)
set(COIN_SOLVER "AUTO" CACHE STRING "COIN-OR solver back-end")
include(../cmake_local/Solver.cmake)

# ---- Create standalone executable ----

file(GLOB sources CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp
)

add_executable(${PROJECT_NAME} ${sources})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "gtopt")

target_include_directories(
  ${PROJECT_NAME} SYSTEM PUBLIC "${COIN_INCLUDE_DIRS}"
)

target_link_libraries(
  ${PROJECT_NAME}
  gtopt::gtopt
  "${COIN_OSI_LIBRARIES};${SOLVER_LIBRARIES}"
)

# ---- RPATH configuration ----
# Collect the directories of non-system shared libraries that the executable
# links against (transitively through gtopt::gtopt) so the installed binary
# can find them at runtime.
# Arrow and Parquet imported targets live in the gtopt sub-project scope, so
# we query the gtopt library target's transitive link libraries instead.

set(_system_lib_dirs "^(/usr/lib|/usr/lib64|/lib|/lib64|/usr/lib/x86_64-linux-gnu)$")
set(_install_rpaths "")
get_target_property(_gtopt_libs gtopt LINK_LIBRARIES)
foreach(_item ${_gtopt_libs})
  if(TARGET ${_item})
    get_target_property(_type ${_item} TYPE)
    if(_type MATCHES "SHARED_LIBRARY|UNKNOWN_LIBRARY")
      get_target_property(_loc ${_item} IMPORTED_LOCATION)
      if(NOT _loc)
        get_target_property(_configs ${_item} IMPORTED_CONFIGURATIONS)
        if(_configs)
          list(GET _configs 0 _first_cfg)
          string(TOUPPER "${_first_cfg}" _uc)
          get_target_property(_loc ${_item} IMPORTED_LOCATION_${_uc})
        endif()
      endif()
      if(_loc AND EXISTS "${_loc}")
        get_filename_component(_dir "${_loc}" DIRECTORY)
        if(NOT _dir MATCHES "${_system_lib_dirs}")
          list(APPEND _install_rpaths "${_dir}")
        endif()
      endif()
    endif()
  elseif(_item AND EXISTS "${_item}")
    # Plain library path (e.g. /opt/coinor/lib/libOsiClp.so)
    get_filename_component(_dir "${_item}" DIRECTORY)
    if(NOT _dir MATCHES "${_system_lib_dirs}")
      list(APPEND _install_rpaths "${_dir}")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES _install_rpaths)

if(_install_rpaths)
  set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "${_install_rpaths}")
endif()

# ---- Install ----
# Only install the binary (and needed runtime libraries), not headers.

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# ---- End-to-end integration tests ----

enable_testing()

set(CASES_DIR "${CMAKE_CURRENT_LIST_DIR}/../cases")
set(CMAKE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(${CMAKE_SCRIPTS_DIR}/add_e2e_case.cmake)

# ---- Register e2e test cases ----
# To add a new case, place it under cases/<name>/ with a system JSON file and
# an output/ directory containing the expected CSV results, then add a line:
#   add_e2e_case(<name> <system_json>)

add_e2e_case(c0 system_c0.json)

