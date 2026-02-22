cmake_minimum_required(VERSION 3.31.6)

project(gtoptIntegrationTest LANGUAGES CXX)

# ---- Options ----

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(../cmake/CompilerCheck.cmake)

option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first" ON)
option(USE_CCACHE "use ccache" YES)
option(ENABLE_TEST_COVERAGE "Enable test coverage instrumentation" OFF)

cmake_policy(SET CMP0167 NEW)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Force Debug build ----

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---- No install rules ----

#set(CMAKE_SKIP_INSTALL_RULES TRUE)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

include(../cmake/ClangFormat.cmake)

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

# ---- Build the gtopt executable ----

file(GLOB sources CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/[a-zA-Z]*.cpp
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

# ---- Code coverage ----

if(ENABLE_TEST_COVERAGE)
  target_compile_options(
    ${PROJECT_NAME} PUBLIC -O0 -g -fprofile-arcs -ftest-coverage
    -fprofile-update=atomic
  )
  target_link_options(${PROJECT_NAME} PUBLIC -fprofile-arcs -ftest-coverage)

  if(TARGET gtopt)
    target_compile_options(
      gtopt PRIVATE -O0 -g -fprofile-arcs -ftest-coverage
      -fprofile-update=atomic
    )                           #
    target_link_options(gtopt PRIVATE -fprofile-arcs -ftest-coverage)
  endif()
endif()

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
add_e2e_case(c1 system_c1.json)
add_e2e_case(ieee_9b ieee_9b.json)
add_e2e_case(ieee_14b ieee_14b.json)
add_e2e_case(ieee_9b_ori ieee_9b_ori.json)
add_e2e_case(ieee_14b_ori ieee_14b_ori.json)
add_e2e_case(ieee_4b_ori ieee_4b_ori.json)
