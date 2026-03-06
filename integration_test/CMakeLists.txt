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

# ---- Build type default ----
# Only set when building integration_test as a standalone top-level project
# (i.e. cmake -S integration_test).  When included from all/CMakeLists.txt the
# build type has already been set at the top level, so we must not force-override
# it here.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(
    CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel
  )
  message(STATUS "Build type not set — defaulting to Debug")
endif()

# ---- No install rules ----

#set(CMAKE_SKIP_INSTALL_RULES TRUE)

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

# ---- Build or reuse the gtopt executable ----
# When included from all/CMakeLists.txt the standalone binary is already
# built as target 'gtoptStandalone'.  Reuse it to avoid recompilation.
# GTOPT_EXECUTABLE_TARGET is consumed by add_e2e_case() in add_e2e_case.cmake
# to reference the correct binary via $<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>.
if(TARGET gtoptStandalone)
  set(GTOPT_EXECUTABLE_TARGET gtoptStandalone)
else()
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
      )
      target_link_options(gtopt PRIVATE -fprofile-arcs -ftest-coverage)
    endif()
  endif()
endif()

# ---- End-to-end integration tests ----

enable_testing()

set(CASES_DIR "${CMAKE_CURRENT_LIST_DIR}/../cases")
set(CMAKE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(${CMAKE_SCRIPTS_DIR}/add_e2e_case.cmake)

# ---- Python / compare_pandapower for pandapower comparison tests ----
#
# Detection priority (first match wins):
#
# 1. User explicitly sets -DCOMPARE_PANDAPOWER_PROGRAM=/path/to/compare_pandapower
#    (a pre-installed entry-point from 'pip install -e scripts/').
#
# 2. The installed 'compare_pandapower' command is found via find_program.
#    This is the preferred route: the entry-point script has the real Python
#    interpreter path baked in, so it is immune to pyenv/virtualenv shim issues.
#    Install with: pip install -e scripts/   (from the repo root)
#
# 3. Fallback: find Python3 that has pandapower installed and use the per-case
#    compare_pandapower.py script.  Override the auto-detected interpreter with
#    -DPANDAPOWER_PYTHON=/path/to/python3 when auto-detection fails.
#    The real interpreter path is resolved via sys.executable to avoid pyenv shims.

set(COMPARE_PANDAPOWER_PROGRAM "" CACHE FILEPATH
  "Path to the compare_pandapower command (from 'pip install -e scripts/')")

if(NOT COMPARE_PANDAPOWER_PROGRAM)
  find_program(_found_compare_pandapower compare_pandapower)
  if(_found_compare_pandapower)
    set(COMPARE_PANDAPOWER_PROGRAM "${_found_compare_pandapower}")
    message(STATUS "Found compare_pandapower command: ${COMPARE_PANDAPOWER_PROGRAM}")
  endif()
endif()

if(NOT COMPARE_PANDAPOWER_PROGRAM)
  # Fallback: find Python3 with pandapower
  set(PANDAPOWER_PYTHON "" CACHE FILEPATH
    "Python interpreter with pandapower (used when compare_pandapower is not installed)")
  if(NOT PANDAPOWER_PYTHON)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import pandapower"
        RESULT_VARIABLE _pandapower_import_result
        OUTPUT_QUIET
        ERROR_QUIET
      )
      if(_pandapower_import_result EQUAL 0)
        # Resolve the real interpreter path to avoid pyenv/shim issues:
        # sys.executable always returns the actual Python binary, not a shim.
        execute_process(
          COMMAND "${Python3_EXECUTABLE}" -c
            "import sys; print(sys.executable, end='')"
          OUTPUT_VARIABLE _real_python_exec
          OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(PANDAPOWER_PYTHON "${_real_python_exec}")
        message(STATUS "Found Python with pandapower: ${PANDAPOWER_PYTHON}")
      else()
        message(STATUS
          "Python3 found but pandapower is not installed — "
          "skipping pandapower comparison tests. "
          "Install scripts with 'pip install -e scripts/' (preferred) or "
          "set -DPANDAPOWER_PYTHON=/path/to/python3 with pandapower available.")
      endif()
    endif()
  endif()
endif()
include(${CMAKE_SCRIPTS_DIR}/add_pandapower_comparison.cmake)

# ---- Register e2e test cases ----
# To add a new case, place it under cases/<name>/ with a system JSON file and
# an output/ directory containing the expected CSV results, then add a line:
#   add_e2e_case(<name> <system_json>)
# To also compare against pandapower, add:
#   add_pandapower_comparison(<name>)   (requires compare_pandapower.py in the case dir)

add_e2e_case(c0 system_c0.json)
add_e2e_case(c1 system_c1.json)
add_e2e_case(ieee_9b ieee_9b.json)
add_e2e_case(ieee_14b ieee_14b.json)
add_e2e_case(ieee_9b_ori ieee_9b_ori.json)
add_e2e_case(ieee_14b_ori ieee_14b_ori.json)
add_e2e_case(ieee_4b_ori ieee_4b_ori.json)
add_e2e_case(s1b s1b.json)
add_e2e_case(bat_4b bat_4b.json)
add_e2e_case(ieee30b ieee30b.json)
add_e2e_case(ieee_57b ieee_57b.json)
add_e2e_case(bat_4b_24 bat_4b_24.json)
add_e2e_case(exp_gen_4b exp_gen_4b.json)
add_e2e_case(exp_bat_4b exp_bat_4b.json)
add_e2e_case(exp_bat_4b_24 exp_bat_4b_24.json)

# ---- Pandapower comparison tests ----
add_pandapower_comparison(s1b)
add_pandapower_comparison(ieee_4b_ori)
add_pandapower_comparison(ieee30b)
add_pandapower_comparison(ieee_57b)
add_pandapower_comparison(bat_4b_24)

# ---- Target: generate pandapower network JSON files ----
# Run 'cmake --build . --target generate-pandapower-nets' (requires compare_pandapower
# and pandapower to be installed) to save a pandapower_net.json file into each
# case directory.  The saved files are then used automatically by the
# add_pandapower_comparison() tests via --pandapower-file.
if(COMPARE_PANDAPOWER_PROGRAM)
  foreach(_pp_case s1b ieee_4b_ori ieee30b ieee_57b)
    list(APPEND _generate_pandapower_commands
      COMMAND "${COMPARE_PANDAPOWER_PROGRAM}"
        --case "${_pp_case}"
        --save-pandapower-file "${CASES_DIR}/${_pp_case}/pandapower_net.json"
    )
  endforeach()

  add_custom_target(generate-pandapower-nets
    ${_generate_pandapower_commands}
    COMMENT "Generating pandapower network JSON files in cases/ directories"
    VERBATIM
  )
endif()
