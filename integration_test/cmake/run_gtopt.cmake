cmake_minimum_required(VERSION 3.14)

# run_gtopt.cmake - Run the gtopt binary for integration testing
#
# Usage:
#   cmake -DGTOPT_BINARY=<path> -DINPUT_FILE=<path> -DOUTPUT_DIR=<path>
#         -DWORKING_DIR=<path> -P run_gtopt.cmake
#
# Creates the output directory, runs the gtopt binary, and checks the exit code.

if(NOT EXISTS "${GTOPT_BINARY}")
  message(FATAL_ERROR "gtopt binary does not exist: ${GTOPT_BINARY}")
endif()

if(NOT EXISTS "${INPUT_FILE}")
  message(FATAL_ERROR "Input file does not exist: ${INPUT_FILE}")
endif()

if(NOT EXISTS "${WORKING_DIR}")
  message(FATAL_ERROR "Working directory does not exist: ${WORKING_DIR}")
endif()

# Create a clean output directory
if(EXISTS "${OUTPUT_DIR}")
  file(REMOVE_RECURSE "${OUTPUT_DIR}")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

message(STATUS "Running gtopt: ${GTOPT_BINARY}")
message(STATUS "  Input:  ${INPUT_FILE}")
message(STATUS "  Output: ${OUTPUT_DIR}")
message(STATUS "  CWD:    ${WORKING_DIR}")

execute_process(
  COMMAND "${GTOPT_BINARY}"
    "${INPUT_FILE}"
    --output-directory "${OUTPUT_DIR}"
    --quiet
  WORKING_DIRECTORY "${WORKING_DIR}"
  RESULT_VARIABLE exit_code
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
)

if(NOT exit_code EQUAL 0)
  message(FATAL_ERROR
    "gtopt exited with code ${exit_code}\n"
    "stdout: ${stdout}\n"
    "stderr: ${stderr}")
endif()

message(STATUS "gtopt completed successfully (exit code: ${exit_code})")

# --- Reassemble hive-sharded CSVs into flat files for e2e comparison ---
# Since commit fc005441 gtopt writes per-(scene, phase) CSV shards named
# `<field>_s<N>_p<M>.csv`.  The e2e golden fixtures and the compare_csv
# validator expect a single flat `<field>.csv`.  Walk the output tree and
# synthesise the flat file by concatenating all shards of the same stem
# (one header, then the data rows of every shard in sorted order).
file(GLOB_RECURSE _shards RELATIVE "${OUTPUT_DIR}"
  "${OUTPUT_DIR}/*_s[0-9]*_p[0-9]*.csv"
  "${OUTPUT_DIR}/*_s[0-9]*_p[0-9]*.csv.gz"
  "${OUTPUT_DIR}/*_s[0-9]*_p[0-9]*.csv.zst"
)

set(_stems_seen "")
foreach(_rel ${_shards})
  # Strip `_s<N>_p<M>` suffix and compression extension to recover the
  # canonical field stem.  "Bus/balance_dual_s0_p0.csv" → "Bus/balance_dual.csv".
  string(REGEX REPLACE
    "_s[0-9]+_p[0-9]+(\\.csv(\\.(gz|zst))?)$" "\\1"
    _flat "${_rel}")
  if(_flat IN_LIST _stems_seen)
    continue()
  endif()
  list(APPEND _stems_seen "${_flat}")

  # Only reassemble plain CSV for now — compressed variants are rarely
  # tested in golden fixtures.
  if(NOT _flat MATCHES "\\.csv$")
    continue()
  endif()

  # Collect every shard belonging to this stem.
  get_filename_component(_dir "${_rel}" DIRECTORY)
  get_filename_component(_stem "${_flat}" NAME_WE)
  file(GLOB _stem_shards
    "${OUTPUT_DIR}/${_dir}/${_stem}_s[0-9]*_p[0-9]*.csv")
  list(SORT _stem_shards)

  set(_out "${OUTPUT_DIR}/${_flat}")
  set(_first TRUE)
  file(WRITE "${_out}" "")
  foreach(_shard ${_stem_shards})
    file(READ "${_shard}" _content)
    if(_first)
      file(APPEND "${_out}" "${_content}")
      set(_first FALSE)
    else()
      # Drop the header line of subsequent shards.
      string(REGEX REPLACE "^[^\n]*\n" "" _content_body "${_content}")
      file(APPEND "${_out}" "${_content_body}")
    endif()
  endforeach()
endforeach()
