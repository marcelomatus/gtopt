#[=======================================================================[.rst:
AddE2ECase
----------

Provides the ``add_e2e_case`` function for registering end-to-end
integration tests that exercise the gtopt executable against reference
case directories.

Each case directory (under ``CASES_DIR``) must contain:

* A system JSON input file.
* An ``output/`` subdirectory with the expected CSV results.
* Optionally, ``output_<solver>/`` directories with solver-specific
  expected results for cases with degenerate LP solutions (where
  different solvers produce equally-valid but numerically different
  primal solutions).

The function creates the following CTest tests for every registered case:

1. **e2e_<case>_solve** – runs the gtopt binary on the input.
2. **e2e_<case>_validate_solution** – checks solution structure / status.
3. **e2e_<case>_compare_<csv>** – compares each expected CSV against actual
   output (one test per file).

When ``GTOPT_SOLVER`` is set (via environment or CMake variable) and a
matching ``output_<solver>/`` directory exists, that directory is used for
golden-file comparison.  Otherwise, ``output/`` is used.

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
  cmake_parse_arguments(ARG "" "" "LABELS" ${ARGN})

  # Resolve the gtopt binary reference.  Prefer GTOPT_EXECUTABLE_FILE (may
  # be a genex like $<TARGET_FILE:gtoptStandalone> or a literal path);
  # fall back to GTOPT_EXECUTABLE_TARGET or ${PROJECT_NAME} for callers
  # that still rely on the old contract.
  if(NOT DEFINED GTOPT_EXECUTABLE_FILE)
    if(NOT DEFINED GTOPT_EXECUTABLE_TARGET)
      set(GTOPT_EXECUTABLE_TARGET "${PROJECT_NAME}")
    endif()
    set(GTOPT_EXECUTABLE_FILE "$<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>")
  endif()
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
      -DGTOPT_BINARY=${GTOPT_EXECUTABLE_FILE}
      -DINPUT_FILE=${input_file}
      -DOUTPUT_DIR=${test_output}
      -DWORKING_DIR=${case_dir}
      -P ${CMAKE_SCRIPTS_DIR}/run_gtopt.cmake
    WORKING_DIRECTORY "${case_dir}"
  )

  # Apply labels to solve test
  if(ARG_LABELS)
    set_tests_properties(e2e_${case_name}_solve PROPERTIES LABELS "${ARG_LABELS}")
  endif()

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
  if(ARG_LABELS)
    set_tests_properties(e2e_${case_name}_validate_solution
      PROPERTIES LABELS "${ARG_LABELS}")
  endif()

  # Test 3: compare each expected CSV file against actual output
  # Use solver-specific golden files when available
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
        -DCASE_DIR=${case_dir}
        -DCSV_REL_PATH=${csv_file}
        -P ${CMAKE_SCRIPTS_DIR}/compare_csv.cmake
    )
    set_tests_properties(e2e_${case_name}_compare_${test_suffix}
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
    if(ARG_LABELS)
      set_tests_properties(e2e_${case_name}_compare_${test_suffix}
        PROPERTIES LABELS "${ARG_LABELS}")
    endif()
  endforeach()
endfunction()
