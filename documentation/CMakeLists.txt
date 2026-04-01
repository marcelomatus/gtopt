cmake_minimum_required(VERSION 3.31.6)

project(GtoptDocs)

# ---- Shared cmake module path ----
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
     "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Dependencies ----

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake")

CPMAddPackage("gh:mosra/m.css#a0d292ec311b97fefd21e93cdefb60f88d19ede6")

include(PatchMcss)
patch_mcss_skip_concepts("${m.css_SOURCE_DIR}")

# When built as part of the all/ super-project the gtopt target already
# exists and gtopt_VERSION is set.  When built standalone (e.g. the Docs CI
# job) we only need the version number for the Doxyfile — there is no need
# to pull in the full gtopt project with all its dependencies (Arrow,
# COIN-OR, Parquet, Boost, …).
if(NOT DEFINED gtopt_VERSION)
  file(READ "${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt" _gtopt_cml)
  string(REGEX MATCH "project\\([^)]*VERSION ([0-9]+[.0-9]*)" _match "${_gtopt_cml}")
  set(gtopt_VERSION "${CMAKE_MATCH_1}")
endif()

# ---- Doxygen variables ----

# set Doxyfile variables
set(DOXYGEN_PROJECT_NAME Gtopt)
set(DOXYGEN_PROJECT_VERSION ${gtopt_VERSION})
set(DOXYGEN_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/doxygen")

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/Doxyfile ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
)

configure_file(${CMAKE_CURRENT_LIST_DIR}/conf.py ${CMAKE_CURRENT_BINARY_DIR}/conf.py)

add_custom_target(
  GenerateDocs
  ${CMAKE_COMMAND} -E make_directory "${DOXYGEN_OUTPUT_DIRECTORY}"
  COMMAND "${m.css_SOURCE_DIR}/documentation/doxygen.py"
          "${CMAKE_CURRENT_BINARY_DIR}/conf.py"
  COMMAND echo "Docs written to: ${DOXYGEN_OUTPUT_DIRECTORY}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
