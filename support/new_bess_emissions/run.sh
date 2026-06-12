#!/usr/bin/env bash
# Invoke gtopt on the assembled new_bess_emissions case.
#
# The case is split across two planning JSON files so the BESS overlay
# stays self-documenting; gtopt merges them at parse time via repeated
# -s flags.  Per `System::merge` (source/system.cpp:822), array entries
# are appended (not deduped by uid), so the BESS UIDs (10009..10021,
# above the PLP max at 10008) and bus references (integer uids) are
# pre-resolved by build.sh to avoid collisions.
#
# Layout the gtopt binary expects:
#   gtopt_plp_plexos_2_years/        ← PLP-base directory (system JSON
#     gtopt_plp_plexos_2_years.json      + per-class Parquet inputs +
#     input/                              boundary_cuts.csv etc.)
#     boundary_cuts.csv
#     ...
#   bess_battery_array.json          ← overlay (no input/ files needed —
#                                       all-scalar Battery fields)
#
# Solver: defaults to CPLEX (priority cplex > highs > cbc > clp); pass
# --solver to override.
set -euo pipefail
CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
GTOPT_BIN="${GTOPT_BIN:-/home/marce/git/gtopt/build/standalone/gtopt}"

cd "${CASE_DIR}/gtopt_plp_plexos_2_years"
exec "${GTOPT_BIN}" \
    -s gtopt_plp_plexos_2_years.json \
    -s "${CASE_DIR}/bess_battery_array.json" \
    --solver cplex \
    --constraint-mode debug \
    "$@"
