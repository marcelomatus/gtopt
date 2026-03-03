#[=======================================================================[.rst:
AddPandapowerComparison
-----------------------

Provides ``add_pandapower_comparison`` for registering a CTest step that
runs a ``compare_pandapower.py`` script after ``e2e_<case>_solve``.

The comparison script receives ``--gtopt-output <dir>`` pointing to the
directory produced by ``run_gtopt.cmake``, builds the equivalent network
in pandapower, runs DC OPF, and exits non-zero on mismatch.

Required variables (set before including this file):
``CASES_DIR``       Top-level cases directory.
``CMAKE_SCRIPTS_DIR`` Directory containing run_gtopt.cmake etc.

Usage:
^^^^^^^
.. code-block:: cmake

   find_package(Python3 REQUIRED COMPONENTS Interpreter)
   include(AddPandapowerComparison)
   add_pandapower_comparison(s1b)
   add_pandapower_comparison(ieee_4b_ori)

#]=======================================================================]

include_guard(GLOBAL)

function(add_pandapower_comparison case_name)
  if(NOT PANDAPOWER_PYTHON)
    message(STATUS
      "No python with pandapower found — skipping comparison for '${case_name}'. "
      "Re-run cmake with -DPANDAPOWER_PYTHON=/path/to/python3 to enable.")
    return()
  endif()

  set(script "${CASES_DIR}/${case_name}/compare_pandapower.py")
  if(NOT EXISTS "${script}")
    message(WARNING
      "Skipping pandapower comparison for '${case_name}': "
      "script not found: ${script}")
    return()
  endif()

  set(gtopt_output "${CMAKE_CURRENT_BINARY_DIR}/test_output/${case_name}")

  add_test(
    NAME e2e_${case_name}_compare_pandapower
    COMMAND "${PANDAPOWER_PYTHON}" "${script}"
      --gtopt-output "${gtopt_output}"
    WORKING_DIRECTORY "${CASES_DIR}/${case_name}"
  )
  set_tests_properties(e2e_${case_name}_compare_pandapower
    PROPERTIES DEPENDS e2e_${case_name}_solve
  )
endfunction()
