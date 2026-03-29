# add_sddp_case.cmake - Register an SDDP integration test case
#
# Usage:
#   add_sddp_case(<case_name> <system_json>
#     [MAX_ITERATIONS <n>] [ALLOWED_EXIT_CODES <list>])
#
# Registers two CTest tests:
#   e2e_<case_name>_sddp_solve    - run gtopt in SDDP mode
#   e2e_<case_name>_sddp_validate - validate sddp_status.json and solution.csv

function(add_sddp_case case_name system_json)
  cmake_parse_arguments(ARG "" "MAX_ITERATIONS" "ALLOWED_EXIT_CODES" ${ARGN})

  if(NOT DEFINED ARG_MAX_ITERATIONS)
    set(ARG_MAX_ITERATIONS 1)
  endif()

  if(NOT DEFINED ARG_ALLOWED_EXIT_CODES)
    set(ARG_ALLOWED_EXIT_CODES "0")
  endif()

  set(case_dir "${CASES_DIR}/${case_name}")
  set(input_file "${case_dir}/${system_json}")
  set(test_output "${CMAKE_CURRENT_BINARY_DIR}/test_output/${case_name}_sddp")

  # Escape semicolons so the list survives as a single -D argument
  string(REPLACE ";" "\\;" _escaped_exit_codes "${ARG_ALLOWED_EXIT_CODES}")

  # Test 1: run gtopt SDDP
  add_test(
    NAME e2e_${case_name}_sddp_solve
    COMMAND ${CMAKE_COMMAND}
      -DGTOPT_BINARY=$<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>
      -DINPUT_FILE=${input_file}
      -DOUTPUT_DIR=${test_output}
      -DWORKING_DIR=${case_dir}
      -DSDDP_MAX_ITERATIONS=${ARG_MAX_ITERATIONS}
      -DALLOWED_EXIT_CODES=${_escaped_exit_codes}
      -P ${CMAKE_SCRIPTS_DIR}/run_sddp_gtopt.cmake
    WORKING_DIRECTORY "${case_dir}"
  )
  set_tests_properties(e2e_${case_name}_sddp_solve PROPERTIES
    TIMEOUT 300
    LABELS "sddp"
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
    LABELS "sddp"
  )
endfunction()
