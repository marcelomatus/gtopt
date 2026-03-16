#[=======================================================================[.rst:
GenerateProfilingReport
-----------------------

cmake ``-P`` script that reads valgrind/callgrind output and ``/usr/bin/time``
timing data, then writes a GitHub-AI–friendly Markdown profiling summary.

Invocation::

  cmake \
    -DTIMING_FILE=<path/to/timing.txt> \
    -DCALLGRIND_FILE=<path/to/callgrind.out> \
    -DCALLGRIND_ANNOTATE_EXECUTABLE=<path> \
    -DOUTPUT_MD=<path/to/profiling-summary.md> \
    -DPROFILING_CASE=<case-directory> \
    -DJUST_BUILD_LP=<ON|OFF> \
    -P profiling/GenerateProfilingReport.cmake

Variables
^^^^^^^^^

``TIMING_FILE``
  Path to the timing output file written by the ``profiling-timing`` target.
  Each line is one ``/usr/bin/time -v`` run.

``CALLGRIND_FILE``
  Path to the ``callgrind.out`` data file.

``CALLGRIND_ANNOTATE_EXECUTABLE``
  Full path to ``callgrind_annotate``.  If empty, the function section is
  omitted from the report.

``OUTPUT_MD``
  Destination for the generated Markdown file.

``PROFILING_CASE``
  Human-readable label for the profiling case (path or name).

``JUST_BUILD_LP``
  ``ON`` or ``OFF`` — whether the profiling run used ``just_build_lp=true``.

#]=======================================================================]

cmake_minimum_required(VERSION 3.19)

# ---- Validate required variables ----------------------------------------
foreach(_var TIMING_FILE OUTPUT_MD)
  if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
    message(FATAL_ERROR "GenerateProfilingReport: ${_var} must be set")
  endif()
endforeach()

# ---- Timestamp ----------------------------------------------------------
string(TIMESTAMP _report_date "%Y-%m-%d %H:%M UTC")

# ---- Parse timing file --------------------------------------------------
set(_wall_times "")
set(_rss_values "")

if(EXISTS "${TIMING_FILE}")
  file(READ "${TIMING_FILE}" _timing_raw)
  string(REPLACE "\n" ";" _timing_lines "${_timing_raw}")

  foreach(_line IN LISTS _timing_lines)
    # /usr/bin/time -v outputs "Elapsed (wall clock) time: H:MM:SS or M:SS.ss"
    if(_line MATCHES "Elapsed.*wall clock.*time[^:]*: *([0-9:.]+)")
      list(APPEND _wall_times "${CMAKE_MATCH_1}")
    endif()
    # "Maximum resident set size (kbytes): NNNN"
    if(_line MATCHES "Maximum resident set size.*: *([0-9]+)")
      list(APPEND _rss_values "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()

list(LENGTH _wall_times _n_reps)

# Compute min/max wall time.
# NOTE: /usr/bin/time -v consistently outputs times in one of these formats:
#   H:MM:SS (e.g. "0:00.73") or "M:SS.ss" — the output is left-padded so that
#   lexicographic (STRLESS/STRGREATER) comparison gives the correct ordering
#   when all values use the same format.  This works reliably for runs up to
#   59 minutes on the same machine.  For longer runs or cross-machine
#   comparison, convert to seconds first.
set(_wall_min "")
set(_wall_max "")
if(_n_reps GREATER 0)
  list(GET _wall_times 0 _wall_min)
  set(_wall_max "${_wall_min}")
  foreach(_t IN LISTS _wall_times)
    if(_t STRLESS _wall_min)
      set(_wall_min "${_t}")
    endif()
    if(_t STRGREATER _wall_max)
      set(_wall_max "${_t}")
    endif()
  endforeach()
endif()

# Max RSS
set(_max_rss "n/a")
if(_rss_values)
  list(GET _rss_values 0 _max_rss)
  foreach(_r IN LISTS _rss_values)
    if(_r GREATER _max_rss)
      set(_max_rss "${_r}")
    endif()
  endforeach()
  # Convert kbytes to MiB for readability
  math(EXPR _max_rss_mb "${_max_rss} / 1024")
  set(_max_rss "${_max_rss_mb} MiB (${_max_rss} kB)")
endif()

# Build timing repetitions table
set(_timing_table "")
if(_wall_times)
  set(_idx 1)
  foreach(_t IN LISTS _wall_times)
    string(APPEND _timing_table "| ${_idx} | ${_t} |\n")
    math(EXPR _idx "${_idx} + 1")
  endforeach()
endif()

# ---- Parse callgrind top functions --------------------------------------
set(_callgrind_section "")
if(CALLGRIND_ANNOTATE_EXECUTABLE AND EXISTS "${CALLGRIND_FILE}")
  execute_process(
    COMMAND
      "${CALLGRIND_ANNOTATE_EXECUTABLE}"
      --auto=no
      --threshold=99
      "${CALLGRIND_FILE}"
    OUTPUT_VARIABLE _cg_raw
    ERROR_VARIABLE _cg_err
    OUTPUT_STRIP_TRAILING_WHITESPACE
    TIMEOUT 60
  )
  # Extract function lines: lines starting with a count followed by a name
  # Format: "  NNNN  function_name  (file)"
  set(_func_lines "")
  string(REPLACE "\n" ";" _cg_lines "${_cg_raw}")
  set(_func_count 0)
  foreach(_line IN LISTS _cg_lines)
    # Lines with call counts look like: "  123,456,789  func (file.cpp:10)"
    if(_line MATCHES "^ *[0-9,]+ +[A-Za-z_].*\\(")
      string(APPEND _func_lines "${_line}\n")
      math(EXPR _func_count "${_func_count} + 1")
      if(_func_count GREATER_EQUAL 30)
        break()
      endif()
    endif()
  endforeach()
  if(_func_lines)
    set(_callgrind_section
      "## Top Functions (callgrind, ≥99% threshold)\n"
      "\n"
      "```\n"
      "${_func_lines}"
      "```\n"
      "\n"
    )
  else()
    set(_callgrind_section
      "## Top Functions (callgrind)\n"
      "\n"
      "> No function data parsed from callgrind output.\n"
      "\n"
    )
  endif()
else()
  if(NOT CALLGRIND_ANNOTATE_EXECUTABLE)
    set(_callgrind_section
      "## Top Functions (callgrind)\n"
      "\n"
      "> `callgrind_annotate` not available — install valgrind.\n"
      "\n"
    )
  else()
    set(_callgrind_section
      "## Top Functions (callgrind)\n"
      "\n"
      "> No callgrind data found at `${CALLGRIND_FILE}`.\n"
      "\n"
    )
  endif()
endif()

# ---- Profiling case label -----------------------------------------------
if(NOT PROFILING_CASE)
  set(_case_label "unknown")
else()
  cmake_path(GET PROFILING_CASE FILENAME _case_label)
endif()
if(NOT JUST_BUILD_LP)
  set(_just_lp "false (full solve)")
else()
  set(_just_lp "true (LP build only)")
endif()

# ---- Compose the Markdown report ----------------------------------------
set(_md
  "# gtopt Profiling Report\n"
  "\n"
  "> Generated: ${_report_date}  \n"
  "> Case: `${_case_label}`  \n"
  "> just_build_lp: ${_just_lp}\n"
  "\n"
  "## Timing Summary\n"
  "\n"
  "| Metric | Value |\n"
  "|--------|-------|\n"
  "| Repetitions | ${_n_reps} |\n"
  "| Wall time (min) | ${_wall_min} |\n"
  "| Wall time (max) | ${_wall_max} |\n"
  "| Max RSS | ${_max_rss} |\n"
  "\n"
)

if(_timing_table)
  string(
    APPEND _md
    "### Per-run Wall Clock Times\n"
    "\n"
    "| Run | Wall Time |\n"
    "|:---:|----------|\n"
    "${_timing_table}"
    "\n"
  )
endif()

list(APPEND _md ${_callgrind_section})

string(
  APPEND _md
  "## How to Reproduce\n"
  "\n"
  "```bash\n"
  "cmake -S profiling -B build-profiling \\\n"
  "  -DPROFILING_CASE=${PROFILING_CASE} \\\n"
  "  -DPROFILING_OUTPUT_DIR=reports/profiling\n"
  "cmake --build build-profiling\n"
  "```\n"
  "\n"
  "## References\n"
  "\n"
  "- Raw callgrind data: `reports/profiling/callgrind.out`\n"
  "- Timing log: `reports/profiling/timing.txt`\n"
  "- [Callgrind manual](https://valgrind.org/docs/manual/cl-manual.html)\n"
)

# ---- Write the report ---------------------------------------------------
file(WRITE "${OUTPUT_MD}" ${_md})
message(STATUS "Profiling report written to: ${OUTPUT_MD}")
if(_n_reps GREATER 0)
  message(STATUS "  Wall time min/max: ${_wall_min} / ${_wall_max}")
  message(STATUS "  Max RSS: ${_max_rss}")
endif()
