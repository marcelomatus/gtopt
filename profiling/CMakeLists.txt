#[=======================================================================[.rst:
gtoptProfiling
--------------

Standalone CMake sub-project that builds gtopt in ``RelWithDebInfo`` mode,
runs it under valgrind/callgrind for a detailed call-graph profile, measures
wall-clock timing over multiple repetitions, and writes a Markdown summary to
``reports/profiling/``.

Quick start::

  # Build and run full profiling analysis:
  cmake -S profiling -B build-profiling \
    [-DPROFILING_CASE="${PWD}/cases/ieee_9b_ori"] \
    [-DPROFILING_OUTPUT_DIR="${PWD}/reports/profiling"] \
    [-DPROFILING_CMAKE_PREFIX_PATH="$(conda info --base)"]
  cmake --build build-profiling

  # Check tool availability:
  cmake --build build-profiling --target check-deps

  # Individual steps:
  cmake --build build-profiling --target profiling-configure
  cmake --build build-profiling --target profiling-build
  cmake --build build-profiling --target profiling-callgrind
  cmake --build build-profiling --target profiling-timing
  cmake --build build-profiling --target profiling-report

Options
^^^^^^^

``PROFILING_CASE``
  Absolute path to the test-case directory.  Must contain at least one
  ``*.json`` file.  Defaults to ``<repo_root>/cases/ieee_9b_ori``.

``PROFILING_BUILD_LP``
  ``ON`` (default) — inject ``"build_lp": true`` into the case JSON so
  that only the LP creation time is measured (no solver overhead).
  Set to ``OFF`` to measure the full solve.

``PROFILING_OUTPUT_DIR``
  Directory where output files are written.  Defaults to
  ``<repo_root>/reports/profiling``.

``PROFILING_REPS``
  Number of ``/usr/bin/time`` timing repetitions.  Default ``5``.

``PROFILING_PYTHON``
  ``ON`` (default) — also run ``python3 -m cProfile`` on plp2gtopt.

``PROFILING_PLP_CASE``
  Relative path under ``scripts/cases/`` to the PLP case for Python
  profiling.  Default ``"plp_dat_ex"``.

``PROFILING_CMAKE_PREFIX_PATH``
  Extra ``CMAKE_PREFIX_PATH`` forwarded to the gtopt configure step.
  Required when Arrow/Parquet are installed via conda.

Targets
^^^^^^^

``profiling`` (ALL)
  Runs the full profiling pipeline in order.

``profiling-configure``
  Configures gtopt in ``RelWithDebInfo`` for accurate profiling.

``profiling-build``
  Builds the gtopt standalone binary.

``profiling-callgrind``
  Runs gtopt under valgrind --tool=callgrind.

``profiling-timing``
  Runs ``PROFILING_REPS`` repetitions timed with ``/usr/bin/time -v``.

``profiling-report``
  Generates ``profiling-summary.md`` from timing + callgrind data.

``profiling-python``
  Runs ``python3 -m cProfile`` on plp2gtopt (only when
  ``PROFILING_PYTHON=ON`` and the case directory exists).

``check-deps``
  Prints the status of all required and optional tools.

#]=======================================================================]

cmake_minimum_required(VERSION 3.31.6)

project(gtoptProfiling LANGUAGES NONE)

# ---- Module path --------------------------------------------------------
list(
  APPEND CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
  "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Options ------------------------------------------------------------
get_filename_component(
  _default_case "${CMAKE_CURRENT_LIST_DIR}/../cases/ieee_9b_ori" ABSOLUTE
)
set(PROFILING_CASE
    "${_default_case}"
    CACHE PATH "Absolute path to the gtopt case directory to profile"
)

option(PROFILING_BUILD_LP
       "Inject build_lp=true into the case JSON (measure LP creation only)" ON
)

get_filename_component(
  _default_output "${CMAKE_CURRENT_LIST_DIR}/../reports/profiling" ABSOLUTE
)
set(PROFILING_OUTPUT_DIR
    "${_default_output}"
    CACHE PATH "Directory where profiling reports are written"
)

set(PROFILING_REPS
    5
    CACHE STRING "Number of timing repetitions for wall-clock measurement"
)

option(PROFILING_PYTHON "Run cProfile on plp2gtopt in addition to C++ profiling" ON)

set(PROFILING_PLP_CASE
    "plp_dat_ex"
    CACHE STRING "Subdirectory under scripts/cases/ to profile with plp2gtopt"
)

set(PROFILING_CMAKE_PREFIX_PATH
    ""
    CACHE PATH
    "Extra CMAKE_PREFIX_PATH for gtopt configure (e.g. conda base for Arrow)"
)

set(PROFILING_EXTRA_CMAKE_ARGS
    ""
    CACHE STRING "Semicolon-separated extra cmake args for profiling-configure"
)

# ---- Find required tools ------------------------------------------------
include(FindValgrind)
find_package(Valgrind REQUIRED)

find_program(GNU_TIME_EXECUTABLE NAMES time PATHS /usr/bin NO_DEFAULT_PATH)
if(NOT GNU_TIME_EXECUTABLE)
  find_program(GNU_TIME_EXECUTABLE NAMES time)
endif()
if(NOT GNU_TIME_EXECUTABLE)
  message(
    WARNING
    "/usr/bin/time (GNU time) not found.  "
    "Install with: sudo apt install time  "
    "The profiling-timing target will be skipped."
  )
endif()

find_program(PYTHON3_EXECUTABLE NAMES python3 python)

# ---- Find the case JSON file at configure time --------------------------
file(GLOB _case_json_files "${PROFILING_CASE}/*.json")
if(NOT _case_json_files)
  message(WARNING "No .json files found in PROFILING_CASE=${PROFILING_CASE}")
  set(_case_json_src "")
else()
  list(GET _case_json_files 0 _case_json_src)
  message(STATUS "Profiling case JSON: ${_case_json_src}")
endif()

# ---- Optionally inject build_lp into a working copy ----------------
if(PROFILING_BUILD_LP AND _case_json_src)
  set(_work_json "${CMAKE_BINARY_DIR}/profile_case.json")
  # Use cmake's JSON support (cmake >= 3.19) to inject build_lp
  file(READ "${_case_json_src}" _json_content)
  # Ensure "options" key exists; SET creates it if absent (cmake >= 3.28)
  string(JSON _json_type ERROR_VARIABLE _json_err TYPE "${_json_content}" "options")
  if(_json_err OR "${_json_type}" STREQUAL "NOTFOUND")
    # "options" missing — insert an empty object first via python3 fallback
    if(PYTHON3_EXECUTABLE)
      execute_process(
        COMMAND
          ${PYTHON3_EXECUTABLE}
          "${CMAKE_CURRENT_LIST_DIR}/inject_build_lp.py"
          "${_case_json_src}"
          "${_work_json}"
        RESULT_VARIABLE _py_rc
      )
      if(NOT _py_rc EQUAL 0)
        message(WARNING "Failed to inject build_lp via python3; using original JSON")
        set(_work_json "${_case_json_src}")
      endif()
    else()
      # Best-effort cmake JSON injection
      string(JSON _json_content SET "${_json_content}" "options" "{}")
      string(JSON _json_content SET "${_json_content}" "options" "build_lp" "true")
      file(WRITE "${_work_json}" "${_json_content}")
    endif()
  else()
    string(JSON _json_content SET "${_json_content}" "options" "build_lp" "true")
    # Override input_directory so external Parquet/CSV files are found
    # relative to the case directory rather than an absolute /tmp path.
    string(JSON _json_content SET "${_json_content}" "options" "input_directory"
           "\"${PROFILING_CASE}\"")
    file(WRITE "${_work_json}" "${_json_content}")
  endif()
  message(STATUS "Profiling JSON (with build_lp=true): ${_work_json}")
elseif(_case_json_src)
  set(_work_json "${_case_json_src}")
endif()

# ---- Paths & directories ------------------------------------------------
set(_repo_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_all_dir "${_repo_root}/all")
set(_profiling_build_dir "${CMAKE_BINARY_DIR}/gtopt-profiling-build")
set(_gtopt_binary "${_profiling_build_dir}/standalone/gtopt")
set(_callgrind_out "${PROFILING_OUTPUT_DIR}/callgrind.out")
set(_timing_file "${PROFILING_OUTPUT_DIR}/timing.txt")
set(_python_output_dir "${PROFILING_OUTPUT_DIR}/python")

file(MAKE_DIRECTORY "${PROFILING_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${_python_output_dir}")

# ---- Process count for parallel builds ----------------------------------
include(ProcessorCount)
ProcessorCount(_nproc)
if(NOT _nproc OR _nproc EQUAL 0)
  set(_nproc 4)
endif()

# ---- Print status -------------------------------------------------------
message(STATUS "=== gtopt Profiling Configuration ===")
message(STATUS "  valgrind:    ${VALGRIND_EXECUTABLE}")
message(STATUS "  callgrind_annotate: ${CALLGRIND_ANNOTATE_EXECUTABLE}")
message(STATUS "  /usr/bin/time: ${GNU_TIME_EXECUTABLE}")
message(STATUS "  python3:     ${PYTHON3_EXECUTABLE}")
message(STATUS "  Case:        ${PROFILING_CASE}")
message(STATUS "  JSON:        ${_work_json}")
message(STATUS "  build_lp: ${PROFILING_BUILD_LP}")
message(STATUS "  Reps:        ${PROFILING_REPS}")
message(STATUS "  Output dir:  ${PROFILING_OUTPUT_DIR}")

# ---- Build configure args list ------------------------------------------
set(_configure_args
  -S "${_all_dir}"
  -B "${_profiling_build_dir}"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_C_COMPILER=clang
  -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_C_COMPILER_LAUNCHER=ccache
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
)
if(PROFILING_CMAKE_PREFIX_PATH)
  list(APPEND _configure_args "-DCMAKE_PREFIX_PATH=${PROFILING_CMAKE_PREFIX_PATH}")
endif()
if(PROFILING_EXTRA_CMAKE_ARGS)
  separate_arguments(_extra_args NATIVE_COMMAND "${PROFILING_EXTRA_CMAKE_ARGS}")
  list(APPEND _configure_args ${_extra_args})
endif()

# ---- Target: check-deps -------------------------------------------------
add_custom_target(
  check-deps
  COMMAND ${CMAKE_COMMAND} -E echo "=== Profiling tool status ==="
  COMMAND ${CMAKE_COMMAND} -E echo "  valgrind:          ${VALGRIND_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  callgrind_annotate: ${CALLGRIND_ANNOTATE_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  /usr/bin/time:     ${GNU_TIME_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo "  python3:           ${PYTHON3_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Install missing tools:"
  COMMAND ${CMAKE_COMMAND} -E echo "  sudo apt install valgrind"
  COMMAND ${CMAKE_COMMAND} -E echo "  sudo apt install time"
  COMMAND ${CMAKE_COMMAND} -E echo "  sudo apt install clang"
  COMMENT "Checking profiling tool dependencies"
)

# ---- Target: profiling-configure ----------------------------------------
add_custom_target(
  profiling-configure
  COMMAND ${CMAKE_COMMAND} ${_configure_args}
  COMMENT "Configuring gtopt in RelWithDebInfo for profiling..."
  USES_TERMINAL
  VERBATIM
)

# ---- Target: profiling-build --------------------------------------------
add_custom_target(
  profiling-build
  COMMAND
    ${CMAKE_COMMAND} --build "${_profiling_build_dir}"
    --target gtoptStandalone
    -j ${_nproc}
  COMMENT "Building gtopt standalone binary for profiling..."
  USES_TERMINAL
  VERBATIM
)
add_dependencies(profiling-build profiling-configure)

# ---- Target: profiling-callgrind ----------------------------------------
if(_work_json)
  # Determine working directory (case directory for relative output paths)
  cmake_path(GET _work_json PARENT_PATH _case_work_dir)
  if(NOT IS_ABSOLUTE "${_case_work_dir}" OR NOT IS_DIRECTORY "${_case_work_dir}")
    set(_case_work_dir "${PROFILING_CASE}")
  endif()

  add_custom_target(
    profiling-callgrind
    COMMAND
      ${VALGRIND_EXECUTABLE}
      --tool=callgrind
      "--callgrind-out-file=${_callgrind_out}"
      --collect-systime=yes
      --simulate-cache=yes
      "${_gtopt_binary}"
      "${_work_json}"
    WORKING_DIRECTORY "${PROFILING_CASE}"
    COMMENT "Profiling with valgrind/callgrind (slow — this can take minutes)..."
    USES_TERMINAL
    VERBATIM
  )
  add_dependencies(profiling-callgrind profiling-build)

  # ---- Target: profiling-timing -----------------------------------------
  if(GNU_TIME_EXECUTABLE)
    # Build a shell script that runs N repetitions and logs timing output.
    # We write it at configure time and invoke it at build time.
    set(_timing_script "${CMAKE_BINARY_DIR}/run-timing.cmake")
    set(_timing_script_content
      [=[
cmake_minimum_required(VERSION 3.19)
set(_gtopt  "@_gtopt_binary@")
set(_json   "@_work_json@")
set(_outfile "@_timing_file@")
set(_reps   "@PROFILING_REPS@")
set(_time   "@GNU_TIME_EXECUTABLE@")
set(_case_dir "@PROFILING_CASE@")

file(WRITE "${_outfile}" "")
message(STATUS "Running ${_reps} timing repetitions...")
foreach(_i RANGE 1 ${_reps})
  message(STATUS "  Repetition ${_i}/${_reps}")
  execute_process(
    COMMAND "${_time}" -v "${_gtopt}" "${_json}"
    WORKING_DIRECTORY "${_case_dir}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
  )
  file(APPEND "${_outfile}" "=== Repetition ${_i} ===\n${_err}\n\n")
endforeach()
message(STATUS "Timing log written to: ${_outfile}")
]=]
    )
    file(WRITE "${_timing_script}" "${_timing_script_content}")
    configure_file("${_timing_script}" "${_timing_script}" @ONLY)

    add_custom_target(
      profiling-timing
      COMMAND ${CMAKE_COMMAND} -P "${_timing_script}"
      COMMENT "Running ${PROFILING_REPS} timing repetitions..."
      USES_TERMINAL
      VERBATIM
    )
    add_dependencies(profiling-timing profiling-build)
  else()
    add_custom_target(
      profiling-timing
      COMMAND ${CMAKE_COMMAND} -E echo
        "Skipping timing: /usr/bin/time not found (sudo apt install time)"
      COMMENT "Timing target skipped (GNU time not available)"
    )
  endif()
else()
  # No case JSON — create stub targets
  foreach(_stub_target profiling-callgrind profiling-timing)
    add_custom_target(
      ${_stub_target}
      COMMAND ${CMAKE_COMMAND} -E echo
        "Skipping ${_stub_target}: no case JSON found in ${PROFILING_CASE}"
      COMMENT "${_stub_target} skipped (no case JSON)"
    )
    add_dependencies(${_stub_target} profiling-build)
  endforeach()
endif()

# ---- Target: profiling-report -------------------------------------------
add_custom_target(
  profiling-report
  COMMAND
    ${CMAKE_COMMAND}
    "-DTIMING_FILE=${_timing_file}"
    "-DCALLGRIND_FILE=${_callgrind_out}"
    "-DCALLGRIND_ANNOTATE_EXECUTABLE=${CALLGRIND_ANNOTATE_EXECUTABLE}"
    "-DOUTPUT_MD=${PROFILING_OUTPUT_DIR}/profiling-summary.md"
    "-DPROFILING_CASE=${PROFILING_CASE}"
    "-DBUILD_LP=${PROFILING_BUILD_LP}"
    "-DPYTHON3_EXECUTABLE=${PYTHON3_EXECUTABLE}"
    -P "${CMAKE_CURRENT_LIST_DIR}/GenerateProfilingReport.cmake"
  COMMENT "Generating Markdown profiling report..."
  USES_TERMINAL
  VERBATIM
)
add_dependencies(profiling-report profiling-callgrind profiling-timing)

# ---- Target: profiling-python -------------------------------------------
set(_plp_case_dir "${_repo_root}/scripts/cases/${PROFILING_PLP_CASE}")
if(PROFILING_PYTHON AND PYTHON3_EXECUTABLE AND IS_DIRECTORY "${_plp_case_dir}")
  set(_py_pstats "${_python_output_dir}/plp2gtopt.pstats")
  set(_py_report "${_python_output_dir}/plp2gtopt_profile.txt")
  set(_py_output_dir_plp "${_python_output_dir}/plp2gtopt_output")

  add_custom_target(
    profiling-python
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_py_output_dir_plp}"
    COMMAND
      ${PYTHON3_EXECUTABLE} -m cProfile
      -o "${_py_pstats}"
      -m plp2gtopt
      --input-dir "${_plp_case_dir}"
      --output-dir "${_py_output_dir_plp}"
    COMMAND
      ${PYTHON3_EXECUTABLE}
      "${CMAKE_CURRENT_LIST_DIR}/generate_pstats_report.py"
      "${_py_pstats}"
      "${_py_report}"
      "30"
    WORKING_DIRECTORY "${_repo_root}"
    COMMENT "Profiling plp2gtopt with cProfile..."
    USES_TERMINAL
    VERBATIM
  )
elseif(PROFILING_PYTHON)
  add_custom_target(
    profiling-python
    COMMAND ${CMAKE_COMMAND} -E echo
      "Skipping Python profiling: case not found at ${_plp_case_dir}"
    COMMENT "Python profiling skipped"
  )
endif()

# ---- Target: profiling (ALL) --------------------------------------------
add_custom_target(profiling ALL)
add_dependencies(profiling profiling-report)
if(TARGET profiling-python)
  add_dependencies(profiling profiling-python)
endif()

# ---- Optional: git-commit target ----------------------------------------
include(GitReports)
add_git_commit_reports_target(
  git-commit-profiling-reports
  "${_repo_root}/reports"
)
