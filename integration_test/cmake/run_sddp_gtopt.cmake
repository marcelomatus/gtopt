cmake_minimum_required(VERSION 3.14)

# run_sddp_gtopt.cmake - Run the gtopt binary in SDDP mode for integration testing
#
# Usage:
#   cmake -DGTOPT_BINARY=<path> -DINPUT_FILE=<path> -DOUTPUT_DIR=<path>
#         -DWORKING_DIR=<path> [-DSDDP_MAX_ITERATIONS=<n>] -P run_sddp_gtopt.cmake
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
    --set sddp_options.min_iterations=1
  WORKING_DIRECTORY "${WORKING_DIR}"
  RESULT_VARIABLE exit_code
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

if(NOT exit_code EQUAL 0)
  message(FATAL_ERROR
    "gtopt SDDP exited with code ${exit_code}\n"
    "stdout: ${stdout}\n"
    "stderr: ${stderr}")
endif()

message(STATUS "gtopt SDDP completed successfully (exit code: ${exit_code})")
