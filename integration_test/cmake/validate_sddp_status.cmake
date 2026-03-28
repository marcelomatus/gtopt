cmake_minimum_required(VERSION 3.14)

# validate_sddp_status.cmake - Validate SDDP solver status output
#
# Usage:
#   cmake -DOUTPUT_DIR=<path> -P validate_sddp_status.cmake
#
# Validates:
#   1. A status file exists (sddp_status.json or monolithic_status.json)
#   2. solution.csv exists and has at least one data row
#   3. The status file contains expected fields

if(NOT EXISTS "${OUTPUT_DIR}")
  message(FATAL_ERROR "Output directory does not exist: ${OUTPUT_DIR}")
endif()

# --- Check status file (SDDP or monolithic fallback) ---
set(sddp_status_file "${OUTPUT_DIR}/sddp_status.json")
set(mono_status_file "${OUTPUT_DIR}/monolithic_status.json")

if(EXISTS "${sddp_status_file}")
  set(status_file "${sddp_status_file}")
  set(status_type "sddp")
  message(STATUS "Found sddp_status.json")
elseif(EXISTS "${mono_status_file}")
  set(status_file "${mono_status_file}")
  set(status_type "monolithic")
  message(STATUS "Found monolithic_status.json (SDDP degraded to monolithic — single phase)")
else()
  message(FATAL_ERROR
    "No status file found in ${OUTPUT_DIR}. "
    "Expected sddp_status.json or monolithic_status.json")
endif()

file(READ "${status_file}" status_content)

# Check that key fields are present
if(status_type STREQUAL "sddp")
  foreach(field "lower_bound" "upper_bound" "gap" "iteration")
    string(FIND "${status_content}" "\"${field}\"" field_pos)
    if(field_pos EQUAL -1)
      message(FATAL_ERROR "sddp_status.json missing expected field: ${field}")
    endif()
  endforeach()
else()
  foreach(field "status" "elapsed_s")
    string(FIND "${status_content}" "\"${field}\"" field_pos)
    if(field_pos EQUAL -1)
      message(FATAL_ERROR "monolithic_status.json missing expected field: ${field}")
    endif()
  endforeach()
endif()

message(STATUS "${status_type} status validated")

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

# Parse header row
list(GET solution_lines 0 header_line)
string(FIND "${header_line}" "status" has_status)
string(FIND "${header_line}" "obj_value" has_obj)

if(has_status EQUAL -1)
  message(FATAL_ERROR "solution.csv missing 'status' column: ${header_line}")
endif()
if(has_obj EQUAL -1)
  message(FATAL_ERROR "solution.csv missing 'obj_value' column: ${header_line}")
endif()

math(EXPR data_rows "${line_count} - 1")
message(STATUS "solution.csv validated: ${data_rows} data row(s)")
message(STATUS "SDDP validation passed")
