cmake_minimum_required(VERSION 3.31.6)

project(gtoptIntegrationTest LANGUAGES CXX)

# ---- Shared cmake module path ----
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
     "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Build defaults ----
include(BuildTypeDefault)
include(CxxStandard)
include(Ccache)

# ---- Options ----
option(ENABLE_TEST_COVERAGE "Enable test coverage instrumentation" OFF)

# ---- Compiler checks and tools ----
include(CompilerCheck)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/tools.cmake")

# ---- Dependencies ----
option(CPM_USE_LOCAL_PACKAGES "Try find_package first" ON)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake")

if(NOT TARGET gtopt::gtopt)
  CPMAddPackage(NAME gtopt SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

include(ClangFormat)

# ---- Standalone binary (or reuse from parent) ----
# When included from all/CMakeLists.txt the standalone binary is already
# built as target 'gtoptStandalone'.  Reuse it to avoid recompilation.
# GTOPT_EXECUTABLE_TARGET is consumed by add_e2e_case() to reference the
# binary via $<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>.
if(TARGET gtoptStandalone)
  set(GTOPT_EXECUTABLE_TARGET gtoptStandalone)
else()
  file(GLOB sources CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_LIST_DIR}/../standalone/source/[a-zA-Z]*.cpp"
  )
  add_executable(${PROJECT_NAME} ${sources})
  set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "gtopt")

  target_link_libraries(
    ${PROJECT_NAME}
    gtopt::gtopt
  )

  include(Coverage)
  if(ENABLE_TEST_COVERAGE)
    target_enable_coverage(${PROJECT_NAME})
    if(TARGET gtopt)
      target_enable_coverage(gtopt PRIVATE)
    endif()
  endif()
endif()

# ---- Testing ----
enable_testing()
include(TestParallelism)

set(CASES_DIR "${CMAKE_CURRENT_LIST_DIR}/../cases")
set(CMAKE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(${CMAKE_SCRIPTS_DIR}/add_e2e_case.cmake)
include(${CMAKE_SCRIPTS_DIR}/add_sddp_case.cmake)

# ---- Pandapower detection ----
# Locate gtopt_compare (installed binary, source tree, or any Python with
# pandapower) for running pandapower comparison tests.
include(${CMAKE_SCRIPTS_DIR}/FindPandapower.cmake)
include(${CMAKE_SCRIPTS_DIR}/add_pandapower_comparison.cmake)

# ---- Register e2e test cases ----
# To add a new case, place it under cases/<name>/ with a system JSON file and
# an output/ directory containing the expected CSV results, then add a line:
#   add_e2e_case(<name> <system_json>)
# To also compare against pandapower, add:
#   add_pandapower_comparison(<name>)

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

# ---- SDDP test cases ----
# To add a new SDDP case, place it under cases/<name>/ with a system JSON file
# that has method="sddp", then add a line:
#   add_sddp_case(<name> <system_json> [MAX_ITERATIONS <n>])
add_sddp_case(sddp_hydro_3phase sddp_hydro_3phase.json MAX_ITERATIONS 3)
add_sddp_case(gtopt_case_2y gtopt_case_2y.json MAX_ITERATIONS 1)
add_sddp_case(gtopt_case_2y_fs gtopt_case_2y_fs.json
  MAX_ITERATIONS 0 ALLOWED_EXIT_CODES "0;1")

# ---- Pandapower comparison tests ----
add_pandapower_comparison(s1b)
add_pandapower_comparison(ieee_4b_ori)
add_pandapower_comparison(ieee30b)
add_pandapower_comparison(ieee_57b)
add_pandapower_comparison(bat_4b_24)

# ---- Target: generate pandapower network JSON files ----
# Run 'cmake --build . --target generate-pandapower-nets' to save a
# pandapower_net.json file into each case directory.  Requires gtopt_compare
# and pandapower to be installed or available in the source tree.
if(COMPARE_PANDAPOWER_PROGRAM OR (COMPARE_PANDAPOWER_PYTHON AND COMPARE_PANDAPOWER_SCRIPTS_DIR))
  foreach(_pp_case s1b ieee_4b_ori ieee30b ieee_57b)
    if(COMPARE_PANDAPOWER_PROGRAM)
      list(APPEND _generate_pandapower_commands
        COMMAND "${COMPARE_PANDAPOWER_PROGRAM}"
          --case "${_pp_case}"
          --save-pandapower-file "${CASES_DIR}/${_pp_case}/pandapower_net.json"
      )
    else()
      list(APPEND _generate_pandapower_commands
        COMMAND ${CMAKE_COMMAND} -E env
          "PYTHONPATH=${COMPARE_PANDAPOWER_SCRIPTS_DIR}"
          "${COMPARE_PANDAPOWER_PYTHON}" -m gtopt_compare
            --case "${_pp_case}"
            --save-pandapower-file "${CASES_DIR}/${_pp_case}/pandapower_net.json"
      )
    endif()
  endforeach()

  add_custom_target(generate-pandapower-nets
    ${_generate_pandapower_commands}
    COMMENT "Generating pandapower network JSON files in cases/ directories"
    VERBATIM
  )
endif()
