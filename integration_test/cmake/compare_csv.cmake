cmake_minimum_required(VERSION 3.14)

# compare_csv.cmake - Compare two CSV files via the Python gtopt_compare_csv tool
#
# Usage:
#   cmake -DACTUAL_FILE=<path> -DEXPECTED_FILE=<path>
#         [-DCASE_DIR=<path> -DCSV_REL_PATH=<rel>]
#         [-DTOLERANCE=<tol>] -P compare_csv.cmake
#
# When CASE_DIR and CSV_REL_PATH are provided, the script checks for a
# solver-specific golden directory (output_<solver>/) matching the
# GTOPT_SOLVER environment variable.  If a solver-specific file exists it
# is used instead of EXPECTED_FILE, enabling exact comparison (--strict).
# When falling back to the generic output/ directory, non-strict mode is
# used to tolerate degenerate LP primal differences.

if(NOT DEFINED TOLERANCE)
  set(TOLERANCE "1e-6")
endif()

if(NOT EXISTS "${ACTUAL_FILE}")
  message(FATAL_ERROR "Actual file does not exist: ${ACTUAL_FILE}")
endif()

# Resolve solver-specific golden file when possible
set(_expected "${EXPECTED_FILE}")
set(_strict "")

if(DEFINED ENV{GTOPT_SOLVER} AND DEFINED CASE_DIR AND DEFINED CSV_REL_PATH)
  set(_solver "$ENV{GTOPT_SOLVER}")
  set(_solver_expected "${CASE_DIR}/output_${_solver}/${CSV_REL_PATH}")
  if(EXISTS "${_solver_expected}")
    set(_expected "${_solver_expected}")
    set(_strict "--strict")
  endif()
endif()

if(NOT EXISTS "${_expected}")
  message(FATAL_ERROR "Expected file does not exist: ${_expected}")
endif()

# Locate the Python comparison tool relative to this script
get_filename_component(_this_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_repo_root "${_this_dir}/../../" ABSOLUTE)
set(_compare_tool "${_repo_root}/tools/gtopt_compare_csv.py")

if(NOT EXISTS "${_compare_tool}")
  message(FATAL_ERROR "Comparison tool not found: ${_compare_tool}")
endif()

find_program(_python NAMES python3 python)
if(NOT _python)
  message(FATAL_ERROR "Python interpreter not found")
endif()

execute_process(
  COMMAND "${_python}" "${_compare_tool}"
    "${ACTUAL_FILE}" "${_expected}"
    --tolerance "${TOLERANCE}"
    ${_strict}
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr
)

if(_stdout)
  message(STATUS "${_stdout}")
endif()

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${_stderr}")
endif()
