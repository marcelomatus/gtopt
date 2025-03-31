cmake_minimum_required(VERSION 3.31.6)

project(gtoptTests LANGUAGES CXX)

# ---- Options ----

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)
option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first"  ON)
option(USE_CCACHE "use ccache" YES)

# --- Import tools ----

#set(USE_STATIC_ANALYZER "cppcheck")
set(USE_CCACHE YES)

include(../cmake/tools.cmake)
option(USE_CCACHE "use ccache" YES)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage("gh:doctest/doctest@2.4.11")

CPMAddPackage("gh:TheLartians/Format.cmake@1.7.3")

if(TEST_INSTALLED_VERSION)
  find_package(gtopt REQUIRED)
else()
  CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

# ---- Create binary ----

file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/source/json/*.cpp
)

add_executable(${PROJECT_NAME} ${sources})
target_link_libraries(${PROJECT_NAME} doctest::doctest gtopt::gtopt)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# enable compiler warnings
if(NOT TEST_INSTALLED_VERSION)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(${PROJECT_NAME} PUBLIC -Wall -Wpedantic -Wextra -Werror)
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
  target_compile_options(${PROJECT_NAME} PUBLIC -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(${PROJECT_NAME} PUBLIC -fprofile-arcs -ftest-coverage)
endif()
