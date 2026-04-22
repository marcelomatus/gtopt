# add_sddp_case.cmake - Register an SDDP integration test case
#
# Usage:
#   add_sddp_case(<case_name> <system_json>
#     [MAX_ITERATIONS <n>]   # default 1
#     [LOW_MEMORY <off|compress|rebuild>]  # default off
#     [TIMEOUT <seconds>]
#     [ALLOWED_EXIT_CODES <list>] [LABELS <list>]
#     [EXTRA_SET <list>]
#     [GOLDEN <file_relative_to_case_dir>])  # optional golden ref
#
# Registers two CTest tests:
#   e2e_<case_name>_sddp_solve     - run gtopt in SDDP mode (parquet output
#                                    + optional --low-memory <mode>)
#   e2e_<case_name>_sddp_validate  - read solver_status.json, solution.csv,
#                                    and the per-element parquet shards with
#                                    `tools/validate_sddp_output.py`, and
#                                    assert tolerance-bounded physical
#                                    invariants (reservoir efin bounds,
#                                    generation ≥ 0, demand shortage ≥ 0, …)

function(add_sddp_case case_name system_json)
  cmake_parse_arguments(
    ARG "RECOVER"
    "MAX_ITERATIONS;LOW_MEMORY;TIMEOUT;CASE_DIR;GOLDEN"
    "ALLOWED_EXIT_CODES;LABELS;EXTRA_SET"
    ${ARGN})

  # ── Resolve the gtopt binary reference ──────────────────────────────────
  if(NOT DEFINED GTOPT_EXECUTABLE_FILE)
    if(NOT DEFINED GTOPT_EXECUTABLE_TARGET)
      set(GTOPT_EXECUTABLE_TARGET "${PROJECT_NAME}")
    endif()
    set(GTOPT_EXECUTABLE_FILE "$<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>")
  endif()

  # ── Defaults ───────────────────────────────────────────────────────────
  if(NOT DEFINED ARG_MAX_ITERATIONS)
    set(ARG_MAX_ITERATIONS 1)
  endif()
  if(NOT DEFINED ARG_LOW_MEMORY)
    set(ARG_LOW_MEMORY "off")
  endif()
  if(NOT DEFINED ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  if(NOT DEFINED ARG_ALLOWED_EXIT_CODES)
    set(ARG_ALLOWED_EXIT_CODES "0")
  endif()

  if(ARG_CASE_DIR)
    set(case_dir "${CASES_DIR}/${ARG_CASE_DIR}")
  else()
    set(case_dir "${CASES_DIR}/${case_name}")
  endif()
  set(input_file "${case_dir}/${system_json}")
  set(test_output "${CMAKE_CURRENT_BINARY_DIR}/test_output/${case_name}_sddp")

  # ── Assemble the EXTRA_SET list so the solve step emits parquet output
  # and honours the requested low_memory mode.  Callers that already set
  # one of these keys in ARG_EXTRA_SET override the defaults.
  set(_extra_set ${ARG_EXTRA_SET})
  set(_have_fmt FALSE)
  set(_have_comp FALSE)
  foreach(_kv IN LISTS _extra_set)
    if(_kv MATCHES "^output_format=")
      set(_have_fmt TRUE)
    endif()
    if(_kv MATCHES "^output_compression=")
      set(_have_comp TRUE)
    endif()
  endforeach()
  if(NOT _have_fmt)
    list(APPEND _extra_set "output_format=parquet")
  endif()
  if(NOT _have_comp)
    list(APPEND _extra_set "output_compression=zstd")
  endif()

  # Escape semicolons so the list survives as a single -D argument
  string(REPLACE ";" "\\;" _escaped_exit_codes "${ARG_ALLOWED_EXIT_CODES}")
  string(REPLACE ";" "\\;" _escaped_extra_set "${_extra_set}")

  # ── Solve step ──────────────────────────────────────────────────────────
  set(_recover_arg "")
  if(ARG_RECOVER)
    set(_recover_arg "-DRECOVER=ON")
  endif()
  add_test(
    NAME e2e_${case_name}_sddp_solve
    COMMAND ${CMAKE_COMMAND}
      -DGTOPT_BINARY=${GTOPT_EXECUTABLE_FILE}
      -DINPUT_FILE=${input_file}
      -DOUTPUT_DIR=${test_output}
      -DWORKING_DIR=${case_dir}
      -DSDDP_MAX_ITERATIONS=${ARG_MAX_ITERATIONS}
      -DLOW_MEMORY=${ARG_LOW_MEMORY}
      -DALLOWED_EXIT_CODES=${_escaped_exit_codes}
      -DEXTRA_SET=${_escaped_extra_set}
      ${_recover_arg}
      -P ${CMAKE_SCRIPTS_DIR}/run_sddp_gtopt.cmake
    WORKING_DIRECTORY "${case_dir}"
  )

  set(_labels "sddp")
  if(ARG_LABELS)
    list(APPEND _labels ${ARG_LABELS})
  endif()

  set_tests_properties(e2e_${case_name}_sddp_solve PROPERTIES
    TIMEOUT ${ARG_TIMEOUT}
    LABELS "${_labels}"
  )

  # ── Validate step (Python) ─────────────────────────────────────────────
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(_python "${Python3_EXECUTABLE}")
  else()
    set(_python "python3")
  endif()
  set(_validator "${CMAKE_SOURCE_DIR}/tools/validate_sddp_output.py")
  # When built standalone from integration_test/, CMAKE_SOURCE_DIR points
  # at integration_test itself rather than the repo root.  Fall back to
  # locating the script one level up.
  if(NOT EXISTS "${_validator}")
    set(_validator "${CMAKE_SOURCE_DIR}/../tools/validate_sddp_output.py")
  endif()

  set(_validator_args
      --output-dir "${test_output}"
      --input-json "${input_file}"
      --max-iterations "${ARG_MAX_ITERATIONS}")
  if(ARG_GOLDEN)
    set(_golden_path "${case_dir}/${ARG_GOLDEN}")
    list(APPEND _validator_args --golden-json "${_golden_path}")
  endif()

  add_test(
    NAME e2e_${case_name}_sddp_validate
    COMMAND "${_python}" "${_validator}" ${_validator_args}
  )
  # The validator reads `$GTOPT_SOLVER` at runtime to pick a
  # solver-specific golden variant (`golden_iter50_clp.json`, etc.)
  # when one is checked in.  ctest inherits the calling shell's
  # environment, so setting `GTOPT_SOLVER=clp` before `ctest` is
  # enough; no explicit ENVIRONMENT property needed.  Falls back to
  # the shared golden when no variant file exists.
  set_tests_properties(e2e_${case_name}_sddp_validate PROPERTIES
    DEPENDS e2e_${case_name}_sddp_solve
    LABELS "${_labels}"
  )
endfunction()
