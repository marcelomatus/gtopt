cmake_minimum_required(VERSION 3.14)

# run_sddp_gtopt.cmake - Run the gtopt binary in SDDP mode for integration testing
#
# Usage:
#   cmake -DGTOPT_BINARY=<path> -DINPUT_FILE=<path> -DOUTPUT_DIR=<path>
#         -DWORKING_DIR=<path> [-DSDDP_MAX_ITERATIONS=<n>]
#         [-DALLOWED_EXIT_CODES=<list>]
#         -P run_sddp_gtopt.cmake
#
# Creates the output directory, runs gtopt with SDDP options, and checks exit code.

if(NOT EXISTS "${GTOPT_BINARY}")
  message(FATAL_ERROR "gtopt binary does not exist: ${GTOPT_BINARY}")
endif()

if(NOT EXISTS "${INPUT_FILE}")
  message(FATAL_ERROR "Input file does not exist: ${INPUT_FILE}")
endif()

if(NOT EXISTS "${WORKING_DIR}")
  message(FATAL_ERROR "Working directory does not exist: ${WORKING_DIR}")
endif()

if(NOT DEFINED SDDP_MAX_ITERATIONS)
  set(SDDP_MAX_ITERATIONS 1)
endif()

if(NOT DEFINED ALLOWED_EXIT_CODES)
  set(ALLOWED_EXIT_CODES "0")
endif()

# Build extra --set arguments from EXTRA_SET list
set(_extra_set_args "")
if(DEFINED EXTRA_SET AND NOT "${EXTRA_SET}" STREQUAL "")
  foreach(_kv IN LISTS EXTRA_SET)
    list(APPEND _extra_set_args "--set" "${_kv}")
  endforeach()
endif()

# --low-memory CLI flag (pass-through from add_sddp_case(LOW_MEMORY ...))
set(_low_memory_args "")
if(DEFINED LOW_MEMORY AND NOT "${LOW_MEMORY}" STREQUAL ""
   AND NOT "${LOW_MEMORY}" STREQUAL "off")
  list(APPEND _low_memory_args "--low-memory" "${LOW_MEMORY}")
endif()

# --recover CLI flag (pass-through from add_sddp_case(RECOVER ON)).
# Without this flag `recovery_mode` is force-pinned to `none` in
# main_options.hpp, so setting `sddp_options.recovery_mode=...`
# via `--set` is not enough to actually load cuts on hot-start.
set(_recover_args "")
if(DEFINED RECOVER AND RECOVER)
  list(APPEND _recover_args "--recover")
endif()

# Create a clean output directory
if(EXISTS "${OUTPUT_DIR}")
  file(REMOVE_RECURSE "${OUTPUT_DIR}")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

message(STATUS "Running gtopt SDDP: ${GTOPT_BINARY}")
message(STATUS "  Input:          ${INPUT_FILE}")
message(STATUS "  Output:         ${OUTPUT_DIR}")
message(STATUS "  CWD:            ${WORKING_DIR}")
message(STATUS "  Max iterations: ${SDDP_MAX_ITERATIONS}")

execute_process(
  COMMAND "${GTOPT_BINARY}"
    "${INPUT_FILE}"
    --set output_directory=${OUTPUT_DIR}
    --set sddp_options.max_iterations=${SDDP_MAX_ITERATIONS}
    ${_low_memory_args}
    ${_recover_args}
    ${_extra_set_args}
  WORKING_DIRECTORY "${WORKING_DIR}"
  RESULT_VARIABLE exit_code
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

# Write exit code to a file so the validate step can check it
file(WRITE "${OUTPUT_DIR}/solve_exit_code.txt" "${exit_code}")

# Check if exit code is in the allowed list
list(FIND ALLOWED_EXIT_CODES "${exit_code}" _idx)
if(_idx EQUAL -1)
  message(FATAL_ERROR
    "gtopt SDDP exited with code ${exit_code} (allowed: ${ALLOWED_EXIT_CODES})\n"
    "stdout: ${stdout}\n"
    "stderr: ${stderr}")
endif()

message(STATUS "gtopt SDDP completed (exit code: ${exit_code})")
