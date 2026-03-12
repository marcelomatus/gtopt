#[=======================================================================[.rst:
AddPandapowerComparison
-----------------------

Provides ``add_pandapower_comparison`` for registering a CTest step that
compares gtopt output against a pandapower DC OPF reference after
``e2e_<case>_solve``.

Detection priority (set by the including CMakeLists.txt):

1. ``COMPARE_PANDAPOWER_PROGRAM`` — path explicitly set by the user.
   Invoked as::

     gtopt_compare --case <name> --gtopt-output <dir>

2. ``COMPARE_PANDAPOWER_PYTHON`` + ``COMPARE_PANDAPOWER_SCRIPTS_DIR`` — Python
   interpreter and path to the ``scripts/`` source directory.  The test is run
   as a module (no installation required)::

     PYTHONPATH=<COMPARE_PANDAPOWER_SCRIPTS_DIR> \
       <COMPARE_PANDAPOWER_PYTHON> -m gtopt_compare \
         --case <name> --gtopt-output <dir>

   This is the preferred route in a development checkout: it works before
   ``pip install -e scripts/`` has been executed, and is not affected by
   ``cmake --build build --target uninstall``.

3. ``COMPARE_PANDAPOWER_PROGRAM`` auto-detected — installed ``gtopt_compare``
   entry-point found via ``find_program``.  Used as a fallback when no
   ``scripts/`` source directory is available.

4. ``PANDAPOWER_PYTHON`` — fallback Python interpreter that has pandapower
   installed.  Used together with the per-case ``gtopt_compare.py``
   script found in ``${CASES_DIR}/<case>/gtopt_compare.py``.

External pandapower network files:
   When ``${CASES_DIR}/<case>/pandapower_net.json`` exists the test
   automatically passes ``--pandapower-file <path>`` so the comparison uses
   the saved network instead of rebuilding it from scratch.  This allows the
   test to work with any external pandapower network (not just the built-in
   ones) and makes the comparison independent of pandapower's built-in case
   loaders.

   Save a network file with::

     gtopt_compare --case <name> --save-pandapower-file \\
         ${CASES_DIR}/<case>/pandapower_net.json

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

  # Optional: use a pre-saved pandapower network JSON if present in the case dir.
  set(_pp_file_args "")
  set(_pp_net_json "${CASES_DIR}/${case_name}/pandapower_net.json")
  if(EXISTS "${_pp_net_json}")
    set(_pp_file_args "--pandapower-file" "${_pp_net_json}")
  endif()

  if(COMPARE_PANDAPOWER_PROGRAM)
    # Preferred: use the installed gtopt_compare entry-point.
    # The entry-point has the real Python path baked in, so it works
    # correctly across pyenv, conda, and system Python installations.
    add_test(
      NAME e2e_${case_name}_gtopt_compare
      COMMAND "${COMPARE_PANDAPOWER_PROGRAM}"
        --case "${case_name}"
        --gtopt-output "${gtopt_output}"
        ${_pp_file_args}
    )
    set_tests_properties(e2e_${case_name}_gtopt_compare
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
  elseif(COMPARE_PANDAPOWER_PYTHON AND COMPARE_PANDAPOWER_SCRIPTS_DIR)
    # Source-tree fallback: run the gtopt_compare package as a Python
    # module directly from the scripts/ directory (no installation required).
    add_test(
      NAME e2e_${case_name}_gtopt_compare
      COMMAND ${CMAKE_COMMAND} -E env
        "PYTHONPATH=${COMPARE_PANDAPOWER_SCRIPTS_DIR}"
        "${COMPARE_PANDAPOWER_PYTHON}" -m gtopt_compare
          --case "${case_name}"
          --gtopt-output "${gtopt_output}"
          ${_pp_file_args}
    )
    set_tests_properties(e2e_${case_name}_gtopt_compare
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
  elseif(PANDAPOWER_PYTHON)
    # Fallback: run the per-case gtopt_compare.py with the found Python.
    set(script "${CASES_DIR}/${case_name}/gtopt_compare.py")
    if(NOT EXISTS "${script}")
      message(WARNING
        "Skipping pandapower comparison for '${case_name}': "
        "script not found: ${script}")
      return()
    endif()

    add_test(
      NAME e2e_${case_name}_gtopt_compare
      COMMAND "${PANDAPOWER_PYTHON}" "${script}"
        --gtopt-output "${gtopt_output}"
        ${_pp_file_args}
      WORKING_DIRECTORY "${CASES_DIR}/${case_name}"
    )
    set_tests_properties(e2e_${case_name}_gtopt_compare
      PROPERTIES DEPENDS e2e_${case_name}_solve
    )
  else()
    message(STATUS
      "No gtopt_compare command or Python with pandapower found — "
      "skipping comparison for '${case_name}'. "
      "Install with 'pip install -e scripts/' (preferred), run from the "
      "source tree (scripts/gtopt_compare must exist and pandapower "
      "must be installed), or set -DPANDAPOWER_PYTHON=/path/to/python3 "
      "to enable.")
  endif()
endfunction()
