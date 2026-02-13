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

option(COIN_USE_CLP "Use Clp" ON)
option(COIN_USE_CBC "Use Cbc" OFF)
option(COIN_USE_CPX "Use Cplex" OFF)

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

# ---- End-to-end integration tests ----

enable_testing()

set(CASES_DIR "${CMAKE_CURRENT_LIST_DIR}/../cases")
set(CMAKE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")

# add_e2e_case(<case_name> <system_json>)
#
# Registers end-to-end integration tests for a case directory under cases/.
# The case directory must contain the given system JSON file and an output/
# subdirectory with the expected CSV results.
#
# Example:
#   add_e2e_case(c0 system_c0.json)
#   add_e2e_case(c1 system_c1.json)
#
function(add_e2e_case case_name system_json)
  set(case_dir "${CASES_DIR}/${case_name}")
  set(input_file "${case_dir}/${system_json}")
  set(expected_dir "${case_dir}/output")
  set(test_output "${CMAKE_CURRENT_BINARY_DIR}/test_output/${case_name}")

  if(NOT EXISTS "${input_file}")
    message(WARNING "Skipping e2e case '${case_name}': input file not found: ${input_file}")
    return()
  endif()

  if(NOT EXISTS "${expected_dir}")
    message(WARNING "Skipping e2e case '${case_name}': expected output dir not found: ${expected_dir}")
    return()
  endif()

  # Test 1: run gtopt on the case and produce output
  add_test(
    NAME e2e_${case_name}_solve
    COMMAND ${CMAKE_COMMAND}
      -DGTOPT_BINARY=$<TARGET_FILE:${PROJECT_NAME}>
      -DINPUT_FILE=${input_file}
      -DOUTPUT_DIR=${test_output}
      -DWORKING_DIR=${case_dir}
      -P ${CMAKE_SCRIPTS_DIR}/run_gtopt.cmake
    WORKING_DIRECTORY "${case_dir}"
  )

  # Test 2: validate the solution output structure and status
  add_test(
    NAME e2e_${case_name}_validate_solution
    COMMAND ${CMAKE_COMMAND}
      -DOUTPUT_DIR=${test_output}
      -DEXPECTED_DIR=${expected_dir}
      -P ${CMAKE_SCRIPTS_DIR}/validate_solution.cmake
  )
  set_tests_properties(e2e_${case_name}_validate_solution
    PROPERTIES DEPENDS e2e_${case_name}_solve
  )

  # Test 3: compare each expected CSV file against actual output
  file(GLOB_RECURSE expected_csvs
    RELATIVE "${expected_dir}"
    "${expected_dir}/*.csv"
  )

  foreach(csv_file ${expected_csvs})
    string(REPLACE "/" "_" test_suffix "${csv_file}")
    string(REPLACE ".csv" "" test_suffix "${test_suffix}")
    add_test(
      NAME e2e_${case_name}_compare_${test_suffix}
      COMMAND ${CMAKE_COMMAND}
        -DACTUAL_FILE=${test_output}/${csv_file}
        -DEXPECTED_FILE=${expected_dir}/${csv_file}
        -P ${CMAKE_SCRIPTS_DIR}/compare_csv.cmake
    )
    set_tests_properties(e2e_${case_name}_compare_${test_suffix}
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
  endforeach()
endfunction()

# ---- Register e2e test cases ----
# To add a new case, place it under cases/<name>/ with a system JSON file and
# an output/ directory containing the expected CSV results, then add a line:
#   add_e2e_case(<name> <system_json>)

add_e2e_case(c0 system_c0.json)

