#[=======================================================================[.rst:
AddPandapowerComparison
-----------------------

Provides ``add_pandapower_comparison`` for registering a CTest step that
compares gtopt output against a pandapower DC OPF reference after
``e2e_<case>_solve``.

Detection priority (set by the including CMakeLists.txt):

1. ``COMPARE_PANDAPOWER_PROGRAM`` — path to the installed ``compare_pandapower``
   entry-point (from ``pip install -e scripts/``).  Invoked as::

     compare_pandapower --case <name> --gtopt-output <dir>

   This is the preferred route because the entry-point script has the real
   Python interpreter baked in, avoiding pyenv / virtualenv shim issues.

2. ``PANDAPOWER_PYTHON`` — fallback Python interpreter that has pandapower
   installed.  Used together with the per-case ``compare_pandapower.py``
   script found in ``${CASES_DIR}/<case>/compare_pandapower.py``.

Required variables (set before including this file):
``CASES_DIR``          Top-level cases directory.
``CMAKE_SCRIPTS_DIR``  Directory containing run_gtopt.cmake etc.

Usage:
^^^^^^^
.. code-block:: cmake

   include(AddPandapowerComparison)
   add_pandapower_comparison(s1b)
   add_pandapower_comparison(ieee_4b_ori)
   add_pandapower_comparison(ieee30b)

#]=======================================================================]

include_guard(GLOBAL)

function(add_pandapower_comparison case_name)
  set(gtopt_output "${CMAKE_CURRENT_BINARY_DIR}/test_output/${case_name}")

  if(COMPARE_PANDAPOWER_PROGRAM)
    # Preferred: use the installed compare_pandapower entry-point.
    # The entry-point has the real Python path baked in, so it works
    # correctly across pyenv, conda, and system Python installations.
    add_test(
      NAME e2e_${case_name}_compare_pandapower
      COMMAND "${COMPARE_PANDAPOWER_PROGRAM}"
        --case "${case_name}"
        --gtopt-output "${gtopt_output}"
    )
    set_tests_properties(e2e_${case_name}_compare_pandapower
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
  elseif(PANDAPOWER_PYTHON)
    # Fallback: run the per-case compare_pandapower.py with the found Python.
    set(script "${CASES_DIR}/${case_name}/compare_pandapower.py")
    if(NOT EXISTS "${script}")
      message(WARNING
        "Skipping pandapower comparison for '${case_name}': "
        "script not found: ${script}")
      return()
    endif()

    add_test(
      NAME e2e_${case_name}_compare_pandapower
      COMMAND "${PANDAPOWER_PYTHON}" "${script}"
        --gtopt-output "${gtopt_output}"
      WORKING_DIRECTORY "${CASES_DIR}/${case_name}"
    )
    set_tests_properties(e2e_${case_name}_compare_pandapower
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
  else()
    message(STATUS
      "No compare_pandapower command or Python with pandapower found — "
      "skipping comparison for '${case_name}'. "
      "Install with 'pip install -e scripts/' (preferred) or "
      "set -DPANDAPOWER_PYTHON=/path/to/python3 to enable.")
  endif()
endfunction()
