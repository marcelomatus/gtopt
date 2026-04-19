cmake_minimum_required(VERSION 3.31.6)

# LANGUAGES NONE — defer the C++ compiler probe.  In external-binary mode
# (the common case) this sub-project compiles nothing, so requiring a working
# clang/gcc at configure time would be pure friction.  enable_language(CXX)
# is called below only if we fall through to the self-contained fallback.
project(gtoptIntegrationTest LANGUAGES NONE)

# ---- Shared cmake module path ----
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
     "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Build defaults (language-agnostic) ----
include(BuildTypeDefault)

# ---- Options ----
option(ENABLE_TEST_COVERAGE "Enable test coverage instrumentation" OFF)

# Absolute path to a prebuilt gtopt binary.  When non-empty, the gtopt
# library and standalone sources are NOT compiled in this build tree — the
# CTest cases invoke the external binary directly.  If left empty the
# resolver below tries $ENV{GTOPT_BIN} and the conventional
# ../build/standalone/gtopt location before falling back to a local build.
set(GTOPT_BINARY "" CACHE FILEPATH
    "Absolute path to a prebuilt gtopt binary (auto-detected if empty)")

# ---- Resolve the gtopt binary to drive the tests ----
# Precedence (first hit wins):
#   1. GTOPT_BINARY explicitly set via -D or cache.
#   2. Target 'gtoptStandalone' already defined (e.g. when included from
#      all/CMakeLists.txt alongside standalone/).
#   3. Auto-detection: $ENV{GTOPT_BIN}, then ../build/standalone/gtopt
#      relative to this directory.
#   4. Self-contained fallback: compile the standalone sources into this
#      build tree so the sub-project remains usable from a fresh checkout.
#
# GTOPT_EXECUTABLE_FILE is consumed by add_e2e_case()/add_sddp_case() and
# resolves to either a generator expression ($<TARGET_FILE:...>) or a
# literal absolute path, transparently substituted into add_test() commands.
set(_gtopt_mode "")

if(GTOPT_BINARY)
  if(NOT EXISTS "${GTOPT_BINARY}")
    message(FATAL_ERROR
      "GTOPT_BINARY is set but does not exist: ${GTOPT_BINARY}")
  endif()
  set(GTOPT_EXECUTABLE_FILE "${GTOPT_BINARY}")
  set(_gtopt_mode "explicit")
elseif(TARGET gtoptStandalone)
  set(GTOPT_EXECUTABLE_TARGET gtoptStandalone)
  set(GTOPT_EXECUTABLE_FILE "$<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>")
  set(_gtopt_mode "in-tree target")
else()
  # Auto-detection.
  set(_gtopt_candidates "")
  if(DEFINED ENV{GTOPT_BIN})
    list(APPEND _gtopt_candidates "$ENV{GTOPT_BIN}")
  endif()
  list(APPEND _gtopt_candidates
       "${CMAKE_CURRENT_LIST_DIR}/../build/standalone/gtopt")

  foreach(_c IN LISTS _gtopt_candidates)
    if(EXISTS "${_c}" AND NOT IS_DIRECTORY "${_c}")
      get_filename_component(GTOPT_BINARY "${_c}" REALPATH)
      set(GTOPT_EXECUTABLE_FILE "${GTOPT_BINARY}")
      set(_gtopt_mode "auto-detected")
      break()
    endif()
  endforeach()
  unset(_gtopt_candidates)
endif()

if(_gtopt_mode STREQUAL "explicit")
  message(STATUS
    "Integration tests: using explicit GTOPT_BINARY=${GTOPT_BINARY}")
elseif(_gtopt_mode STREQUAL "in-tree target")
  message(STATUS
    "Integration tests: reusing in-tree gtoptStandalone target "
    "(no recompilation)")
elseif(_gtopt_mode STREQUAL "auto-detected")
  message(STATUS
    "Integration tests: auto-detected prebuilt gtopt at ${GTOPT_BINARY} "
    "(pass -DGTOPT_BINARY=/other/path to override, or remove the file to "
    "force a local build)")
else()
  # Self-contained fallback — compile everything here.
  message(STATUS
    "Integration tests: no prebuilt gtopt found — compiling the binary "
    "in this build tree")

  enable_language(CXX)
  include(CxxStandard)
  include(Ccache)
  include(FastLinker)
  include(CompilerCheck)
  include("${CMAKE_CURRENT_LIST_DIR}/../cmake/tools.cmake")

  option(CPM_USE_LOCAL_PACKAGES "Try find_package first" ON)
  include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake")
  if(NOT TARGET gtopt::gtopt)
    CPMAddPackage(NAME gtopt SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
  endif()

  include(ClangFormat)

  file(GLOB sources CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_LIST_DIR}/../standalone/source/[a-zA-Z]*.cpp"
  )
  add_executable(${PROJECT_NAME} ${sources})
  set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "gtopt")

  target_link_libraries(
    ${PROJECT_NAME}
    gtopt::gtopt
  )

  set(GTOPT_EXECUTABLE_TARGET ${PROJECT_NAME})
  set(GTOPT_EXECUTABLE_FILE "$<TARGET_FILE:${GTOPT_EXECUTABLE_TARGET}>")

  include(Coverage)
  if(ENABLE_TEST_COVERAGE)
    target_enable_coverage(${PROJECT_NAME})
    if(TARGET gtopt)
      target_enable_coverage(gtopt PRIVATE)
    endif()
  endif()
endif()

unset(_gtopt_mode)

# ---- Ensure solver plugins are rebuilt before running tests ----
# Only meaningful when we have an in-tree target to hang dependencies on.
# In external/auto-detected mode the plugins live next to GTOPT_BINARY and
# are managed by the build that produced it.
if(DEFINED GTOPT_EXECUTABLE_TARGET)
  include(GtoptPlugin)
  gtopt_require_solver_plugins(${GTOPT_EXECUTABLE_TARGET})
endif()

# ---- Testing ----
enable_testing()
include(TestParallelism)

set(CASES_DIR "${CMAKE_CURRENT_LIST_DIR}/../cases")
set(CMAKE_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(${CMAKE_SCRIPTS_DIR}/add_e2e_case.cmake)
include(${CMAKE_SCRIPTS_DIR}/add_sddp_case.cmake)

# ---- Pandapower detection ----
# Locate gtopt_compare (installed binary, source tree, or any Python with
# pandapower) for running pandapower comparison tests.
include(${CMAKE_SCRIPTS_DIR}/FindPandapower.cmake)
include(${CMAKE_SCRIPTS_DIR}/add_pandapower_comparison.cmake)

# ---- Options ----
option(GTOPT_SLOW_TESTS "Register slow integration tests (case_2y)" OFF)

# ---- Register e2e test cases ----
# To add a new case, place it under cases/<name>/ with a system JSON file and
# an output/ directory containing the expected CSV results, then add a line:
#   add_e2e_case(<name> <system_json>)
# To also compare against pandapower, add:
#   add_pandapower_comparison(<name>)

add_e2e_case(c0 system_c0.json)
add_e2e_case(c1 system_c1.json)
add_e2e_case(ieee_9b ieee_9b.json)
add_e2e_case(ieee_14b ieee_14b.json)
add_e2e_case(ieee_9b_ori ieee_9b_ori.json)
add_e2e_case(ieee_14b_ori ieee_14b_ori.json)
add_e2e_case(ieee_4b_ori ieee_4b_ori.json)
add_e2e_case(s1b s1b.json)
add_e2e_case(bat_4b bat_4b.json)
add_e2e_case(ieee30b ieee30b.json)
add_e2e_case(ieee_57b ieee_57b.json)
add_e2e_case(bat_4b_24 bat_4b_24.json)
add_e2e_case(exp_gen_4b exp_gen_4b.json)
add_e2e_case(exp_bat_4b exp_bat_4b.json)
add_e2e_case(exp_bat_4b_24 exp_bat_4b_24.json)

# ---- SDDP test cases ----
# To add a new SDDP case, place it under cases/<name>/ with a system JSON file
# that has method="sddp", then add a line:
#   add_sddp_case(<name> <system_json>
#                 [MAX_ITERATIONS <n>]
#                 [LOW_MEMORY <off|compress|rebuild>])
#
# The small sddp_hydro_3phase fixture (1 reservoir, 2 generators, 3 phases)
# is the regression harness for SDDP — every listed case solves with the
# external gtopt binary in parquet output mode and is then validated by
# `tools/validate_sddp_output.py`, which reads the hive-partitioned
# parquet shards and asserts tolerance-bounded physical invariants
# (reservoir efin bounds, generation ≥ 0, demand shortage ≥ 0, optimal
# cells have finite obj_value).
#
# Grid: {MAX_ITERATIONS ∈ {0, 3, 50}} × {LOW_MEMORY ∈ {off, compress, rebuild}}
# — 0 exercises pure sim-pass from loaded cuts, 3 is a quick training
# sanity check, 50 lets SDDP converge so the final UB ≈ LB.  Each
# low_memory variant exercises a different backend-lifecycle path
# (off keeps backends live, compress release + replay from snapshot,
# rebuild re-flattens from collections).

# Per-iteration goldens live next to the case:
#
#   cases/sddp_hydro_3phase/golden_iter0.json   — pure sim pass, no training;
#     hydro runs flat out, reservoir drained; objective_value = 104 700.
#   cases/sddp_hydro_3phase/golden_iter3.json   — this small fixture is
#     easy enough that SDDP converges within 3 iterations, so iter=3
#     goldens match the iter=50 converged state (323 100).
#   cases/sddp_hydro_3phase/golden_iter50.json  — fully converged
#     reference, UB=LB=323 100.
#
# Every case (iter=0 / iter=3 / iter=50, off / compress / rebuild) pins
# its objective_value, per-phase obj_values, and 5 element samples
# (Reservoir/efin_sol, Generator/generation_sol, Bus/balance_dual) so
# a numerical drift in any low_memory mode surfaces immediately.

# max_iter=0 (pure sim pass) across all three low-memory modes.
add_sddp_case(sddp_hydro_3phase_iter0_off sddp_hydro_3phase.json
  CASE_DIR sddp_hydro_3phase MAX_ITERATIONS 0 LOW_MEMORY off
  ALLOWED_EXIT_CODES "0;1"
  GOLDEN golden_iter0.json)
add_sddp_case(sddp_hydro_3phase_iter0_compress sddp_hydro_3phase.json
  CASE_DIR sddp_hydro_3phase MAX_ITERATIONS 0 LOW_MEMORY compress
  ALLOWED_EXIT_CODES "0;1"
  GOLDEN golden_iter0.json)
add_sddp_case(sddp_hydro_3phase_iter0_rebuild sddp_hydro_3phase.json
  CASE_DIR sddp_hydro_3phase MAX_ITERATIONS 0 LOW_MEMORY rebuild
  ALLOWED_EXIT_CODES "0;1"
  GOLDEN golden_iter0.json)

# Quick training sanity check (iter=3) — off-mode only; iter=50 covers
# compress + rebuild below.
add_sddp_case(sddp_hydro_3phase_iter3 sddp_hydro_3phase.json
  CASE_DIR sddp_hydro_3phase MAX_ITERATIONS 3
  GOLDEN golden_iter3.json)

# Converged runs (iter=50) across all three low-memory modes.
add_sddp_case(sddp_hydro_3phase_iter50_off sddp_hydro_3phase.json
  CASE_DIR sddp_hydro_3phase MAX_ITERATIONS 50 LOW_MEMORY off
  GOLDEN golden_iter50.json)
add_sddp_case(sddp_hydro_3phase_iter50_compress sddp_hydro_3phase.json
  CASE_DIR sddp_hydro_3phase MAX_ITERATIONS 50 LOW_MEMORY compress
  GOLDEN golden_iter50.json)
add_sddp_case(sddp_hydro_3phase_iter50_rebuild sddp_hydro_3phase.json
  CASE_DIR sddp_hydro_3phase MAX_ITERATIONS 50 LOW_MEMORY rebuild
  GOLDEN golden_iter50.json)

if(GTOPT_SLOW_TESTS)
  add_sddp_case(gtopt_case_2y gtopt_case_2y.json
    MAX_ITERATIONS 1 TIMEOUT 600 LABELS slow)
  add_sddp_case(gtopt_case_2y_fs gtopt_case_2y_fs.json
    MAX_ITERATIONS 0 ALLOWED_EXIT_CODES "0;1" LABELS slow)
endif()

# ---- Pandapower comparison tests ----
add_pandapower_comparison(s1b)
add_pandapower_comparison(ieee_4b_ori)
add_pandapower_comparison(ieee30b)
add_pandapower_comparison(ieee_57b)
add_pandapower_comparison(bat_4b_24)

# ---- Target: generate pandapower network JSON files ----
# Run 'cmake --build . --target generate-pandapower-nets' to save a
# pandapower_net.json file into each case directory.  Requires gtopt_compare
# and pandapower to be installed or available in the source tree.
if(COMPARE_PANDAPOWER_PROGRAM OR (COMPARE_PANDAPOWER_PYTHON AND COMPARE_PANDAPOWER_SCRIPTS_DIR))
  foreach(_pp_case s1b ieee_4b_ori ieee30b ieee_57b)
    if(COMPARE_PANDAPOWER_PROGRAM)
      list(APPEND _generate_pandapower_commands
        COMMAND "${COMPARE_PANDAPOWER_PROGRAM}"
          --case "${_pp_case}"
          --save-pandapower-file "${CASES_DIR}/${_pp_case}/pandapower_net.json"
      )
    else()
      list(APPEND _generate_pandapower_commands
        COMMAND ${CMAKE_COMMAND} -E env
          "PYTHONPATH=${COMPARE_PANDAPOWER_SCRIPTS_DIR}"
          "${COMPARE_PANDAPOWER_PYTHON}" -m gtopt_compare
            --case "${_pp_case}"
            --save-pandapower-file "${CASES_DIR}/${_pp_case}/pandapower_net.json"
      )
    endif()
  endforeach()

  add_custom_target(generate-pandapower-nets
    ${_generate_pandapower_commands}
    COMMENT "Generating pandapower network JSON files in cases/ directories"
    VERBATIM
  )
endif()
