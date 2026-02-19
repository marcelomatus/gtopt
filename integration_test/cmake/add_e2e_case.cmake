#[=======================================================================[.rst:
AddE2ECase
----------

Provides the ``add_e2e_case`` function for registering end-to-end
integration tests that exercise the gtopt executable against reference
case directories.

Each case directory (under ``CASES_DIR``) must contain:

* A system JSON input file.
* An ``output/`` subdirectory with the expected CSV results.

The function creates the following CTest tests for every registered case:

1. **e2e_<case>_solve** – runs the gtopt binary on the input.
2. **e2e_<case>_validate_solution** – checks solution structure / status.
3. **e2e_<case>_compare_<csv>** – compares each expected CSV against actual
   output (one test per file).

Required variables (must be set before ``include(AddE2ECase)``):

``CASES_DIR``
  Path to the top-level cases directory.

``CMAKE_SCRIPTS_DIR``
  Path to the directory containing ``run_gtopt.cmake``,
  ``validate_solution.cmake`` and ``compare_csv.cmake``.

Example
^^^^^^^

.. code-block:: cmake

   set(CASES_DIR "${CMAKE_CURRENT_LIST_DIR}/../cases")
   set(CMAKE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")
   include(AddE2ECase)
   add_e2e_case(c0 system_c0.json)

#]=======================================================================]

include_guard(GLOBAL)

function(add_e2e_case case_name system_json)
  set(case_dir "${CASES_DIR}/${case_name}")
  set(input_file "${case_dir}/${system_json}")
  set(expected_dir "${case_dir}/output")
  set(test_output "${CMAKE_CURRENT_BINARY_DIR}/test_output/${case_name}")

  if(NOT EXISTS "${input_file}")
    message(WARNING "Skipping e2e case '${case_name}': input file not found: ${input_file}")
    return()
  endif()

  if(NOT EXISTS "${expected_dir}")
    message(WARNING "Skipping e2e case '${case_name}': expected output dir not found: ${expected_dir}")
    return()
  endif()

  # Test 1: run gtopt on the case and produce output
  add_test(
    NAME e2e_${case_name}_solve
    COMMAND ${CMAKE_COMMAND}
      -DGTOPT_BINARY=$<TARGET_FILE:${PROJECT_NAME}>
      -DINPUT_FILE=${input_file}
      -DOUTPUT_DIR=${test_output}
      -DWORKING_DIR=${case_dir}
      -P ${CMAKE_SCRIPTS_DIR}/run_gtopt.cmake
    WORKING_DIRECTORY "${case_dir}"
  )

  # Test 2: validate the solution output structure and status
  add_test(
    NAME e2e_${case_name}_validate_solution
    COMMAND ${CMAKE_COMMAND}
      -DOUTPUT_DIR=${test_output}
      -DEXPECTED_DIR=${expected_dir}
      -P ${CMAKE_SCRIPTS_DIR}/validate_solution.cmake
  )
  set_tests_properties(e2e_${case_name}_validate_solution
    PROPERTIES DEPENDS e2e_${case_name}_solve
  )

  # Test 3: compare each expected CSV file against actual output
  file(GLOB_RECURSE expected_csvs
    RELATIVE "${expected_dir}"
    "${expected_dir}/*.csv"
  )

  foreach(csv_file ${expected_csvs})
    string(REPLACE "/" "_" test_suffix "${csv_file}")
    string(REPLACE ".csv" "" test_suffix "${test_suffix}")
    add_test(
      NAME e2e_${case_name}_compare_${test_suffix}
      COMMAND ${CMAKE_COMMAND}
        -DACTUAL_FILE=${test_output}/${csv_file}
        -DEXPECTED_FILE=${expected_dir}/${csv_file}
        -P ${CMAKE_SCRIPTS_DIR}/compare_csv.cmake
    )
    set_tests_properties(e2e_${case_name}_compare_${test_suffix}
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
  endforeach()
endfunction()
