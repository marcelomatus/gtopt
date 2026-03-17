cmake_minimum_required(VERSION 3.14)

# validate_solution.cmake - Validate gtopt solution output
#
# Usage:
#   cmake -DOUTPUT_DIR=<path> -DEXPECTED_DIR=<path> -P validate_solution.cmake
#
# Validates:
#   1. solution.csv exists and has the consolidated columnar format
#      (header: scene,phase,status,obj_value,kappa)
#   2. All data rows have status 0 (optimal)
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
list(LENGTH solution_lines line_count)

if(line_count LESS 2)
  message(FATAL_ERROR "solution.csv has no data rows (only ${line_count} line(s))")
endif()

# Parse header row to find column indices
list(GET solution_lines 0 header_line)
string(REPLACE "," ";" header_fields "${header_line}")

set(col_scene -1)
set(col_phase -1)
set(col_status -1)
set(col_obj_value -1)
set(col_kappa -1)

set(hdr_idx 0)
foreach(hdr ${header_fields})
  string(STRIP "${hdr}" hdr)
  if(hdr STREQUAL "scene")
    set(col_scene ${hdr_idx})
  elseif(hdr STREQUAL "phase")
    set(col_phase ${hdr_idx})
  elseif(hdr STREQUAL "status")
    set(col_status ${hdr_idx})
  elseif(hdr STREQUAL "obj_value")
    set(col_obj_value ${hdr_idx})
  elseif(hdr STREQUAL "kappa")
    set(col_kappa ${hdr_idx})
  endif()
  math(EXPR hdr_idx "${hdr_idx} + 1")
endforeach()

if(col_obj_value EQUAL -1)
  message(FATAL_ERROR "obj_value column not found in solution.csv header: ${header_line}")
endif()
if(col_status EQUAL -1)
  message(FATAL_ERROR "status column not found in solution.csv header: ${header_line}")
endif()

# Validate each data row
math(EXPR last_line "${line_count} - 1")
foreach(i RANGE 1 ${last_line})
  list(GET solution_lines ${i} data_line)
  string(STRIP "${data_line}" data_line)
  if(data_line STREQUAL "")
    continue()
  endif()

  string(REPLACE "," ";" data_fields "${data_line}")

  list(GET data_fields ${col_status} status_val)
  string(STRIP "${status_val}" status_val)
  if(NOT status_val STREQUAL "0")
    message(FATAL_ERROR "Solution row ${i}: status is not optimal (status=${status_val})")
  endif()

  list(GET data_fields ${col_obj_value} obj_val)
  string(STRIP "${obj_val}" obj_val)
  string(REGEX MATCH "^-?[0-9]+(\\.[0-9]+(e[+-]?[0-9]+)?)?$" is_numeric "${obj_val}")
  if(NOT is_numeric)
    message(FATAL_ERROR "Solution row ${i}: obj_value is not a valid number: '${obj_val}'")
  endif()

  if(col_scene GREATER_EQUAL 0)
    list(GET data_fields ${col_scene} sc_val)
    string(STRIP "${sc_val}" sc_val)
  else()
    set(sc_val "?")
  endif()
  if(col_phase GREATER_EQUAL 0)
    list(GET data_fields ${col_phase} ph_val)
    string(STRIP "${ph_val}" ph_val)
  else()
    set(ph_val "?")
  endif()

  message(STATUS "  scene=${sc_val} phase=${ph_val} status=${status_val} obj_value=${obj_val}")
endforeach()

# --- Check expected output structure ---
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
