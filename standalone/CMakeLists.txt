cmake_minimum_required(VERSION 3.31.6)

project(gtoptStandalone LANGUAGES CXX)

# ---- Options ----

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_BUILD_TYPE Debug)

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)
option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first" ON)
option(USE_CCACHE "use ccache" YES)

cmake_policy(SET CMP0167 NEW)
set(CPPCHECK_ARGS
    "--check-level=exhaustive;--suppress=returnDanglingLifetime;--suppress=internalAstError"
)
# set(USE_STATIC_ANALYZER "clang-tidy;cppcheck")
set(USE_CCACHE YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Boost REQUIRED COMPONENTS program_options)
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})
CPMAddPackage("gh:TheLartians/Format.cmake@1.8.3")

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake_local ${PROJECT_SOURCE_DIR}/../cmake)

option(COIN_USE_CLP "Use Clp" OFF)
option(COIN_USE_CBC "Use Cbc" OFF)
option(COIN_USE_CPX "Use Cplex" ON)

set(COIN_ROOT_DIR
  "/opt/coinor"
  CACHE PATH "Coinor root directory"
)
set(CPLEX_ROOT_DIR
  "/opt/cplex"
  CACHE PATH "CPLEX root directory"
)
include(../cmake_local/Solver.cmake)

# ---- Create standalone executable ----

file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp)

add_executable(${PROJECT_NAME} ${sources})

target_link_libraries(${PROJECT_NAME} gtopt::gtopt Boost::program_options)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 23 OUTPUT_NAME "gtopt")

target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC "${COIN_INCLUDE_DIRS}")
target_link_libraries(
  ${PROJECT_NAME} "${COIN_OSI_LIBRARIES};${SOLVER_LIBRARIES}"
)

