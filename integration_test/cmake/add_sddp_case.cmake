# add_sddp_case.cmake - Register an SDDP integration test case
#
# Usage:
#   add_sddp_case(<case_name> <system_json>
#     [MAX_ITERATIONS <n>] [TIMEOUT <seconds>]
#     [ALLOWED_EXIT_CODES <list>] [LABELS <list>]
#     [EXTRA_SET <list>])
#
# Registers two CTest tests:
#   e2e_<case_name>_sddp_solve    - run gtopt in SDDP mode
#   e2e_<case_name>_sddp_validate - validate solver_status.json and solution.csv

function(add_sddp_case case_name system_json)
  cmake_parse_arguments(ARG "" "MAX_ITERATIONS;TIMEOUT;CASE_DIR" "ALLOWED_EXIT_CODES;LABELS;EXTRA_SET" ${ARGN})

  # Resolve the gtopt binary reference.  Prefer GTOPT_EXECUTABLE_FILE (may
  # be a genex like $<TARGET_FILE:gtoptStandalone> or a literal path);
  # fall back to GTOPT_EXECUTABLE_TARGET for backward compatibility.
  if(NOT DEFINED GTOPT_EXECUTABLE_FILE)
    if(NOT DEFINED GTOPT_EXECUTABLE_TARGET)
      set(GTOPT_EXECUTABLE_TARGET "${PROJECT_NAME}")
    endif()
    set(GTOPT_EXECUTABLE_FILE "$<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>")
  endif()

  if(NOT DEFINED ARG_MAX_ITERATIONS)
    set(ARG_MAX_ITERATIONS 1)
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

  # Escape semicolons so the list survives as a single -D argument
  string(REPLACE ";" "\\;" _escaped_exit_codes "${ARG_ALLOWED_EXIT_CODES}")

  # Test 1: run gtopt SDDP
  add_test(
    NAME e2e_${case_name}_sddp_solve
    COMMAND ${CMAKE_COMMAND}
      -DGTOPT_BINARY=${GTOPT_EXECUTABLE_FILE}
      -DINPUT_FILE=${input_file}
      -DOUTPUT_DIR=${test_output}
      -DWORKING_DIR=${case_dir}
      -DSDDP_MAX_ITERATIONS=${ARG_MAX_ITERATIONS}
      -DALLOWED_EXIT_CODES=${_escaped_exit_codes}
      -DEXTRA_SET=${ARG_EXTRA_SET}
      -P ${CMAKE_SCRIPTS_DIR}/run_sddp_gtopt.cmake
    WORKING_DIRECTORY "${case_dir}"
  )
  # Build label list: always include "sddp", plus any caller-specified labels
  set(_labels "sddp")
  if(ARG_LABELS)
    list(APPEND _labels ${ARG_LABELS})
  endif()

  set_tests_properties(e2e_${case_name}_sddp_solve PROPERTIES
    TIMEOUT ${ARG_TIMEOUT}
    LABELS "${_labels}"
  )

  # Test 2: validate SDDP status and solution
  add_test(
    NAME e2e_${case_name}_sddp_validate
    COMMAND ${CMAKE_COMMAND}
      -DOUTPUT_DIR=${test_output}
      -P ${CMAKE_SCRIPTS_DIR}/validate_sddp_status.cmake
  )
  set_tests_properties(e2e_${case_name}_sddp_validate PROPERTIES
    DEPENDS e2e_${case_name}_sddp_solve
    LABELS "${_labels}"
  )
endfunction()
