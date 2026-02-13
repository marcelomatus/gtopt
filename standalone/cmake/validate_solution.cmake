# validate_solution.cmake - Validate gtopt solution output
#
# Usage:
#   cmake -DOUTPUT_DIR=<path> -DEXPECTED_DIR=<path> -P validate_solution.cmake
#
# Validates:
#   1. solution.csv exists and has correct structure (obj_value, kappa, status)
#   2. Status is 0 (optimal)
#   3. All expected output subdirectories and files exist

if(NOT EXISTS "${OUTPUT_DIR}")
  message(FATAL_ERROR "Output directory does not exist: ${OUTPUT_DIR}")
endif()

if(NOT EXISTS "${EXPECTED_DIR}")
  message(FATAL_ERROR "Expected directory does not exist: ${EXPECTED_DIR}")
endif()

# --- Check solution.csv ---
set(solution_file "${OUTPUT_DIR}/solution.csv")
if(NOT EXISTS "${solution_file}")
  message(FATAL_ERROR "solution.csv not found in output directory: ${OUTPUT_DIR}")
endif()

file(STRINGS "${solution_file}" solution_lines)

# Parse solution.csv - format is "key,value" with possible leading whitespace
set(found_obj_value FALSE)
set(found_status FALSE)

foreach(line ${solution_lines})
  string(STRIP "${line}" line)
  if(line STREQUAL "")
    continue()
  endif()

  string(REPLACE "," ";" fields "${line}")
  list(LENGTH fields field_count)
  if(field_count LESS 2)
    continue()
  endif()

  list(GET fields 0 key)
  list(GET fields 1 value)
  string(STRIP "${key}" key)
  string(STRIP "${value}" value)

  if(key STREQUAL "obj_value")
    set(found_obj_value TRUE)
    string(REGEX MATCH "^-?[0-9]+(\\.[0-9]+(e[+-]?[0-9]+)?)?$" is_numeric "${value}")
    if(NOT is_numeric)
      message(FATAL_ERROR "obj_value is not a valid number: '${value}'")
    endif()
    message(STATUS "  obj_value = ${value}")
  elseif(key STREQUAL "status")
    set(found_status TRUE)
    if(NOT value STREQUAL "0")
      message(FATAL_ERROR "Solution status is not optimal: status=${value}")
    endif()
    message(STATUS "  status = ${value} (optimal)")
  elseif(key STREQUAL "kappa")
    message(STATUS "  kappa = ${value}")
  endif()
endforeach()

if(NOT found_obj_value)
  message(FATAL_ERROR "obj_value not found in solution.csv")
endif()

if(NOT found_status)
  message(FATAL_ERROR "status not found in solution.csv")
endif()

# --- Check expected output structure ---
# Collect expected CSV files from the expected directory
file(GLOB_RECURSE expected_files
  RELATIVE "${EXPECTED_DIR}"
  "${EXPECTED_DIR}/*.csv"
)

foreach(rel_file ${expected_files})
  set(actual_file "${OUTPUT_DIR}/${rel_file}")
  if(NOT EXISTS "${actual_file}")
    message(FATAL_ERROR "Expected output file missing: ${rel_file}")
  endif()
  message(STATUS "  Found: ${rel_file}")
endforeach()

message(STATUS "Solution validation passed")
