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
# Ensure non-system shared libraries (Arrow, Parquet, COIN-OR, etc.) can be
# found at runtime after installation.

include(InstallRpath)
configure_install_rpath(${PROJECT_NAME} gtopt)

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

