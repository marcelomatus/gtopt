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

# ---- Patch m.css to skip C++20 concepts (unsupported) ----
# m.css has zero concept support.  Doxygen generates compound XML files for
# C++ concepts (kind="concept") that trigger an AssertionError in parse_xml().
# Patch: early-return from parse_xml when the compound kind is "concept".
set(_mcss_doxygen_py "${m.css_SOURCE_DIR}/documentation/doxygen.py")
file(READ "${_mcss_doxygen_py}" _mcss_src)
# Only patch once (idempotent for CPM cache hits)
string(FIND "${_mcss_src}" "compound.kind == 'concept'" _already_patched)
if(_already_patched EQUAL -1)
  # Insert a concept skip right after 'compound.kind = compounddef.attrib[...'
  string(REPLACE
    "compound.kind = compounddef.attrib['kind']"
    "compound.kind = compounddef.attrib['kind']\n    # [gtopt patch] skip C++20 concepts — m.css does not support them\n    if compound.kind == 'concept':\n        logging.debug(\"{}: skipping unsupported C++20 concept\".format(state.current))\n        return None"
    _mcss_src "${_mcss_src}"
  )
  file(WRITE "${_mcss_doxygen_py}" "${_mcss_src}")
  message(STATUS "Patched m.css doxygen.py to skip C++20 concepts")
endif()

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
