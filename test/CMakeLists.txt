cmake_minimum_required(VERSION 3.31.6)

project(gtoptTests LANGUAGES CXX)

# ---- Options ----

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_DEBUG "-Wall -O0 -g3 -Wno-deprecated-declarations")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -Wall -O2 -g -Wno-deprecated-declarations")

set(CMAKE_BUILD_TYPE Debug)

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)
option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first" ON)
option(USE_CCACHE "use ccache" YES)

cmake_policy(SET CMP0167 NEW)
set(CPPCHECK_ARGS
    "--check-level=exhaustive;--suppress=returnDanglingLifetime;--suppress=internalAstError"
)
#set(USE_STATIC_ANALYZER "clang-tidy;cppcheck")

set(USE_CCACHE YES)
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


# ---- Create binary ----

file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/source/json/[a-zA-Z]*.cpp
)

add_executable(${PROJECT_NAME} ${sources})

target_link_libraries(${PROJECT_NAME} doctest::doctest gtopt::gtopt)

target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC "${COIN_INCLUDE_DIRS}")
target_link_libraries(
  ${PROJECT_NAME} "${COIN_OSI_LIBRARIES};${SOLVER_LIBRARIES}"
)

# enable compiler warnings
if(NOT TEST_INSTALLED_VERSION)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${PROJECT_NAME} PUBLIC -D_GLIBCXX_CISO646 -Wall -Wpedantic -Wextra -Werror -Wno-c2y-extensions -Wno-deprecated-declarations)
  elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PUBLIC /W4 /WX)
    target_compile_definitions(${PROJECT_NAME} PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
  endif()
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
  )
  target_link_options(${PROJECT_NAME} PUBLIC -fprofile-arcs -ftest-coverage)
endif()
