cmake_minimum_required(VERSION 3.31.6)

project(gtoptStandalone LANGUAGES CXX)

# Default to ~/.local for non-root users (XDG Base Directory)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake_local/DefaultInstallPrefix.cmake")

# ---- Options ----

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TEST_COVERAGE "Enable coverage instrumentation for the gtopt binary" OFF)

include(../cmake/CompilerCheck.cmake)

option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first" ON)
option(USE_CCACHE "use ccache" YES)

cmake_policy(SET CMP0167 NEW)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

if(NOT TARGET gtopt::gtopt)
  CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

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

# ---- RPATH configuration ----
# Ensure non-system shared libraries (Arrow, Parquet, COIN-OR, etc.) can be
# found at runtime after installation.
# INSTALL_RPATH_USE_LINK_PATH automatically adds linker search directories to
# the installed binary's RPATH so that libraries in non-standard locations
# (e.g. /usr/local/lib, /opt/coinor/lib) are found.
# Set the variable BEFORE add_executable so the property is propagated.

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

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

# ---- Code coverage ----
# When ON, instrument the binary so that running it during e2e tests writes
# .gcda files that lcov can collect alongside the unit-test coverage data.

if(ENABLE_TEST_COVERAGE)
  target_compile_options(
    ${PROJECT_NAME} PUBLIC -O0 -g -fprofile-arcs -ftest-coverage
    -fprofile-update=atomic
  )
  target_link_options(${PROJECT_NAME} PUBLIC -fprofile-arcs -ftest-coverage)
endif()

# configure_install_rpath walks the link-library tree of the gtopt library
# target, resolves imported-target locations, and sets INSTALL_RPATH so that
# shared libraries in non-standard directories are found after install.

include(InstallRpath)
configure_install_rpath(${PROJECT_NAME} gtopt)

# ---- Install ----
# Only install the binary (and needed runtime libraries), not headers.

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

