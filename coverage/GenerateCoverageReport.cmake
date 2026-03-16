#[=======================================================================[.rst:
GenerateCoverageReport
----------------------

cmake ``-P`` script that reads an ``lcov`` ``.info`` file, parses the
coverage summary, and writes a GitHub-AI–friendly Markdown report.

Invocation::

  cmake \
    -DLCOV_EXECUTABLE=<path> \
    -DLCOV_INFO_FILE=<path/to/coverage.info> \
    -DOUTPUT_MD=<path/to/coverage-summary.md> \
    -DGCOV_TOOL=<path/to/gcov-wrapper> \
    -DREPO_ROOT=<repo-root-path> \
    -P coverage/GenerateCoverageReport.cmake

Variables
^^^^^^^^^

``LCOV_EXECUTABLE``
  Full path to the ``lcov`` binary.

``LCOV_INFO_FILE``
  Full path to the filtered ``coverage.info`` data file produced by lcov.

``OUTPUT_MD``
  Destination path for the generated Markdown report.

``GCOV_TOOL``
  Path to the gcov wrapper to pass to lcov (``llvm-gcov.sh`` for clang,
  ``gcov-14`` for GCC).

``REPO_ROOT``
  Absolute path to the repository root — used for context in the report.

#]=======================================================================]

cmake_minimum_required(VERSION 3.19)

# ---- Validate required variables ----------------------------------------
foreach(_var LCOV_EXECUTABLE LCOV_INFO_FILE OUTPUT_MD)
  if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
    message(FATAL_ERROR "GenerateCoverageReport: ${_var} must be set")
  endif()
endforeach()

if(NOT EXISTS "${LCOV_INFO_FILE}")
  message(
    WARNING
    "GenerateCoverageReport: ${LCOV_INFO_FILE} does not exist — "
    "writing placeholder report"
  )
  file(
    WRITE "${OUTPUT_MD}"
    "# gtopt Coverage Report\n\n"
    "> ⚠️ No coverage data found (`coverage.info` missing).  "
    "Run `cmake --build build-coverage --target coverage-collect` first.\n"
  )
  return()
endif()

# ---- Run lcov --summary -------------------------------------------------
if(NOT GCOV_TOOL)
  set(GCOV_TOOL "gcov")
endif()

execute_process(
  COMMAND
    "${LCOV_EXECUTABLE}"
    --summary "${LCOV_INFO_FILE}"
    --gcov-tool "${GCOV_TOOL}"
    --rc branch_coverage=1
    --ignore-errors inconsistent,inconsistent
  OUTPUT_VARIABLE _sum_stdout
  ERROR_VARIABLE _sum_stderr
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
)
# lcov prints summary to stderr
set(_summary "${_sum_stdout}${_sum_stderr}")

# ---- Parse summary metrics ----------------------------------------------
string(REGEX MATCH "lines[. ]+: ([0-9.]+)% \\(([^)]+)\\)" _m "${_summary}")
set(_line_pct "${CMAKE_MATCH_1}")
set(_line_detail "${CMAKE_MATCH_2}")

string(REGEX MATCH "functions[. ]+: ([0-9.]+)% \\(([^)]+)\\)" _m "${_summary}")
set(_func_pct "${CMAKE_MATCH_1}")
set(_func_detail "${CMAKE_MATCH_2}")

string(REGEX MATCH "branches[. ]+: ([0-9.]+)% \\(([^)]+)\\)" _m "${_summary}")
set(_branch_pct "${CMAKE_MATCH_1}")
set(_branch_detail "${CMAKE_MATCH_2}")

if("${_line_pct}" STREQUAL "")
  set(_line_pct "n/a")
endif()
if("${_func_pct}" STREQUAL "")
  set(_func_pct "n/a")
endif()
if("${_branch_pct}" STREQUAL "")
  set(_branch_pct "n/a")
endif()

# ---- Run lcov --list for per-file breakdown ------------------------------
execute_process(
  COMMAND
    "${LCOV_EXECUTABLE}"
    --list "${LCOV_INFO_FILE}"
    --gcov-tool "${GCOV_TOOL}"
    --rc branch_coverage=1
    --ignore-errors inconsistent,inconsistent
  OUTPUT_VARIABLE _list_stdout
  ERROR_VARIABLE _list_stderr
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE
)
set(_list_raw "${_list_stdout}${_list_stderr}")

# Build a Markdown table from the --list output.
# lcov --list lines look like:
#   source/foo.cpp        |  75.0% |  80.0% |  60.0% |
set(_file_table "")
string(REPLACE "\n" ";" _list_lines "${_list_raw}")
foreach(_line IN LISTS _list_lines)
  if(_line MATCHES "^([^ ][^|]+)\\| *([0-9.]+%) *\\| *([0-9.]+%) *\\|")
    set(_fname "${CMAKE_MATCH_1}")
    set(_lpct "${CMAKE_MATCH_2}")
    set(_fpct "${CMAKE_MATCH_3}")
    string(STRIP "${_fname}" _fname)
    # Trim common prefixes for readability
    string(REGEX REPLACE "^.*/gtopt/source/" "source/" _fname "${_fname}")
    string(REGEX REPLACE "^.*/gtopt/include/gtopt/" "include/gtopt/" _fname "${_fname}")
    string(APPEND _file_table "| `${_fname}` | ${_lpct} | ${_fpct} |\n")
  endif()
endforeach()

# ---- Identify zero-coverage files ---------------------------------------
set(_zero_files "")
foreach(_line IN LISTS _list_lines)
  if(_line MATCHES "^([^ ][^|]+)\\| *0\\.0% *\\|")
    string(STRIP "${CMAKE_MATCH_1}" _fname)
    string(REGEX REPLACE "^.*/gtopt/" "" _fname "${_fname}")
    string(APPEND _zero_files "- `${_fname}`\n")
  endif()
endforeach()

# ---- Timestamp ----------------------------------------------------------
string(TIMESTAMP _report_date "%Y-%m-%d %H:%M UTC")

# ---- Determine repo name for heading ------------------------------------
if(REPO_ROOT)
  cmake_path(GET REPO_ROOT FILENAME _repo_name)
else()
  set(_repo_name "gtopt")
endif()

# ---- Compose the Markdown report ----------------------------------------
set(_md
  "# ${_repo_name} C++ Coverage Report\n"
  "\n"
  "> Generated: ${_report_date}  \n"
  "> Tool: lcov + genhtml  \n"
  "> Source: `coverage.info`\n"
  "\n"
  "## Summary\n"
  "\n"
  "| Metric | Coverage | Detail |\n"
  "|--------|:--------:|--------|\n"
  "| **Lines** | **${_line_pct}** | ${_line_detail} |\n"
  "| **Functions** | **${_func_pct}** | ${_func_detail} |\n"
  "| **Branches** | **${_branch_pct}** | ${_branch_detail} |\n"
  "\n"
)

if(_file_table)
  string(
    APPEND _md
    "## Per-file Coverage\n"
    "\n"
    "| File | Lines | Functions |\n"
    "|------|:-----:|:---------:|\n"
    "${_file_table}"
    "\n"
  )
endif()

if(_zero_files)
  string(
    APPEND _md
    "## Files with 0% Line Coverage\n"
    "\n"
    "These files have no executed lines and may need additional tests:\n"
    "\n"
    "${_zero_files}"
    "\n"
  )
endif()

string(
  APPEND _md
  "## How to Improve Coverage\n"
  "\n"
  "1. **Add unit tests** for the zero-coverage files listed above.\n"
  "2. **Run locally**: `cmake -S coverage -B build-coverage "
  "&& cmake --build build-coverage`\n"
  "3. **View HTML report**: open `reports/coverage/html/index.html`\n"
  "4. **Regenerate**: delete `build-coverage/` and rerun step 2.\n"
  "\n"
  "## References\n"
  "\n"
  "- HTML report: `reports/coverage/html/index.html`\n"
  "- Raw data: `reports/coverage/coverage.info`\n"
  "- [lcov documentation](https://ltp.sourceforge.net/coverage/lcov.php)\n"
)

# ---- Write the report ---------------------------------------------------
file(WRITE "${OUTPUT_MD}" ${_md})
message(STATUS "Coverage report written to: ${OUTPUT_MD}")
message(STATUS "  Lines:     ${_line_pct} (${_line_detail})")
message(STATUS "  Functions: ${_func_pct} (${_func_detail})")
message(STATUS "  Branches:  ${_branch_pct} (${_branch_detail})")
