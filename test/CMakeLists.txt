cmake_minimum_required(VERSION 3.31.6)

project(gtoptTests LANGUAGES CXX)

# ---- Options ----

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(ENABLE_INTEGRATION_TESTS "Build gtopt binary and run e2e integration tests" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)
option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first" ON)
option(USE_CCACHE "use ccache" YES)

cmake_policy(SET CMP0167 NEW)
set(CPPCHECK_ARGS
    "--check-level=exhaustive;--suppress=returnDanglingLifetime;--suppress=internalAstError"
)
#set(USE_STATIC_ANALYZER "clang-tidy;cppcheck")


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

if(TEST_INSTALLED_VERSION)
  find_package(gtopt REQUIRED)
else()
  CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

CPMAddPackage("gh:doctest/doctest@2.4.12")

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


# ---- Create binary ----

file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/source/json/[a-zA-Z]*.cpp
)

add_executable(${PROJECT_NAME} ${sources})

target_include_directories(
  ${PROJECT_NAME} SYSTEM PUBLIC "${COIN_INCLUDE_DIRS}"
)

target_link_libraries(
  ${PROJECT_NAME}
  doctest::doctest
  gtopt::gtopt
  "${COIN_OSI_LIBRARIES};${SOLVER_LIBRARIES}"
)

# enable compiler warnings
if(NOT TEST_INSTALLED_VERSION)
  include(../cmake_local/CompilerWarnings.cmake)
  target_set_warnings(${PROJECT_NAME})
  target_compile_definitions(
    ${PROJECT_NAME}
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:DOCTEST_CONFIG_USE_STD_HEADERS>"
  )
endif()

# ---- Add gtoptTests ----

enable_testing()

# Note: doctest and similar testing frameworks can automatically configure CMake
# tests. For other testing frameworks add the tests target instead: add_test(NAME
# ${PROJECT_NAME} COMMAND ${PROJECT_NAME})

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(${PROJECT_NAME})

# ---- code coverage ----

if(ENABLE_TEST_COVERAGE)
  target_compile_options(
    ${PROJECT_NAME} PUBLIC -O0 -g -fprofile-arcs -ftest-coverage
    -fprofile-update=atomic
  )
  target_link_options(${PROJECT_NAME} PUBLIC -fprofile-arcs -ftest-coverage)

  # Also instrument the gtopt library itself so that source/*.cpp files
  # are included in the coverage report (not just the headers).
  if(NOT TEST_INSTALLED_VERSION AND TARGET gtopt)
    target_compile_options(
      gtopt PRIVATE -O0 -g -fprofile-arcs -ftest-coverage
      -fprofile-update=atomic
    )
    target_link_options(gtopt PRIVATE -fprofile-arcs -ftest-coverage)
  endif()
endif()

# ---- Integration tests (e2e) ----
#
# When ENABLE_INTEGRATION_TESTS=ON the gtopt solver binary is built here,
# reusing the gtopt::gtopt library already compiled above.  The four e2e
# test cases are then registered so that `ctest` runs them alongside the
# unit tests in a single pass.

if(ENABLE_INTEGRATION_TESTS)
  set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../cmake_local
                        ${CMAKE_CURRENT_LIST_DIR}/../cmake
  )

  set(COIN_ROOT_DIR "/opt/coinor" CACHE PATH "Coinor root directory")
  set(CPLEX_ROOT_DIR "/opt/cplex" CACHE PATH "CPLEX root directory")
  set(COIN_SOLVER "AUTO" CACHE STRING "COIN-OR solver back-end")
  include(../cmake_local/Solver.cmake)

  # Build the gtopt solver executable, linked against the already-built library.
  file(GLOB gtopt_sources CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_LIST_DIR}/../standalone/source/[a-zA-Z]*.cpp
  )

  add_executable(gtopt ${gtopt_sources})

  target_include_directories(gtopt SYSTEM PUBLIC "${COIN_INCLUDE_DIRS}")

  target_link_libraries(
    gtopt
    gtopt::gtopt
    "${COIN_OSI_LIBRARIES};${SOLVER_LIBRARIES}"
  )

  if(ENABLE_TEST_COVERAGE)
    target_compile_options(
      gtopt PUBLIC -O0 -g -fprofile-arcs -ftest-coverage -fprofile-update=atomic
    )
    target_link_options(gtopt PUBLIC -fprofile-arcs -ftest-coverage)
  endif()

  # Register e2e cases via the shared add_e2e_case helper.
  set(CASES_DIR "${CMAKE_CURRENT_LIST_DIR}/../cases")
  set(CMAKE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../integration_test/cmake")

  include(${CMAKE_SCRIPTS_DIR}/add_e2e_case.cmake)

  add_e2e_case(c0 system_c0.json)
  add_e2e_case(c1 system_c1.json)
  add_e2e_case(ieee_9b ieee_9b.json)
  add_e2e_case(ieee_14b ieee_14b.json)
endif()
