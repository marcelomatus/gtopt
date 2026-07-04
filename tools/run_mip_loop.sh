#!/usr/bin/env bash
# Serial MIP loop over the CEN PCP PLEXOS daily cases (support/plexos/pcp_*),
# SLOWEST-FIRST, with PER-CASE empirical line lifts.
#
# Line lifts (support/plexos/per_case_lifts.json): the 35 lines PLEXOS
# over-dispatches in EVERY case (the common base) PLUS each case's own extra
# over-rated lines, captured from the PLEXOS solution (RES bundle).  Passed
# per case as --lift-line-caps so gtopt demotes those EL=1 hard caps to soft
# bands, matching PLEXOS's over-rating dispatch (37-43 lines/case).  Regenerate
# the json with ~/tmp/mip_lifted_run.py (the capture step) if cases change.
#
# Solve config:
#   * BARRIER root — from the tuned cplex.prm (LPMethod=4) that the converter
#     installs into <out>/solvers/; carries Gomory/CutPasses/RINS/HeuristicEffort.
#   * crossover=auto — B&C needs a vertex basis at the root, so we let CPLEX
#     cross over (NOT crossover=none, which leaves an interior point and cripples
#     branch & cut).
#   * mip_start ENABLED — the relax->round->domain_rules->inject warm start.
#     With the per-case lifts its rounded commitment is feasible (the lifts
#     remove the line-overload infeasibility that made it fail un-lifted), so
#     it seeds CPLEX with a good incumbent.
#   * K=10 loss segments + calibrated water-value factors (RALCO 0.80 /
#     COLBUN 0.85, the 2026-06-07 best-aggregate point).
#   * 3h CPLEX time-limit (returns the best incumbent at the cap) + a slightly
#     longer subprocess watchdog (timeout -k) as a hard backstop.
#
# HARDEST-FIRST: the two that did NOT converge under the prior cap (04-12,
# 04-22 — stuck at the root with a ~140% gap) run first, then 05-17 (slowest
# that did converge), then the rest by descending prior solve time.
#
# Env overrides: GTOPT_BIN, BASE, WATER_VALUE_FACTOR, TIME_LIMIT,
# SOLVE_TIMEOUT, PER_CASE_LIFTS, SUPPORT, SCRIPTS.
set -u

SCRIPTS="${SCRIPTS:-/home/marce/git/gtopt/scripts}"
SUPPORT="${SUPPORT:-/home/marce/git/gtopt/support/plexos}"
GTOPT="${GTOPT_BIN:-/home/marce/git/gtopt/build-release/standalone/gtopt}"
BASE="${BASE:-$HOME/tmp/mip_loop_lifted}"
PER_CASE_LIFTS="${PER_CASE_LIFTS:-$SUPPORT/per_case_lifts.json}"
WATER_VALUE_FACTOR="${WATER_VALUE_FACTOR:-COLBUN:0.85,RALCO:0.80}"
TIME_LIMIT="${TIME_LIMIT:-10800}"         # CPLEX wall-clock cap (s) = 3h
SOLVE_TIMEOUT="${SOLVE_TIMEOUT:-11400}"   # subprocess watchdog backstop (s) = 3h10m

# Hardest-first: the 2 non-converged (04-12, 04-22 — ~140% root gap) lead, then
# 05-17 (converged at 9679s), then the rest by descending prior solve time:
#   03-15 3807 | 02-15 2635 | 12-21 1989 | 01-04 1526 | 10-05 1521
#   12-07 1413 | 11-23 814  | 04-07 780  | 01-18 722  | 11-09 418 | 10-19 335
DATES="${DATES:-2026-04-12 2026-04-22 2026-05-17 2026-03-15 2026-02-15 2025-12-21 2026-01-04 2025-10-05 2025-12-07 2025-11-23 2026-04-07 2026-01-18 2025-11-09 2025-10-19}"

mkdir -p "$BASE"
SUMMARY="$BASE/summary.csv"
echo "date,n_lifts,convert_s,convert_ok,solve_s,solve_rc,status,obj_value" > "$SUMMARY"
echo "binary:  $GTOPT"
echo "lifts:   $PER_CASE_LIFTS"
echo "caps:    time_limit=${TIME_LIMIT}s  watchdog=${SOLVE_TIMEOUT}s"
echo "base:    $BASE"
echo "order:   hardest-first ($(echo "$DATES" | wc -w) cases)"

for date in $DATES; do
  d8=${date//-/}
  casedir="$SUPPORT/pcp_$date"
  cdir="$BASE/pcp_$date"; out="$cdir/out"
  rm -rf "$cdir"; mkdir -p "$out"
  echo "================ $date ================"
  datos=$(ls "$casedir"/DATOS"$d8".zip.xz 2>/dev/null || ls "$casedir"/DATOS"$d8".zip 2>/dev/null)
  if [ -z "$datos" ]; then echo "$date,,,0,,,no_datos," >> "$SUMMARY"; echo "  no DATOS"; continue; fi

  # per-case lift list (35 common + this case's own extras), comma-joined
  lifts=$(python3 -c "import json;print(','.join(json.load(open('$PER_CASE_LIFTS')).get('$date',[])))")
  nlift=$(python3 -c "import json;print(len(json.load(open('$PER_CASE_LIFTS')).get('$date',[])))")

  # 1) convert: canonical CEN PCP config + per-case empirical lifts
  conv_args=(--nseg-losses 10 --nseg-tangent 6 --nseg-uniform 4
    --loss-error-pct 1.0 --loss-cost-eps 1.0 --loss-pwl-layout dynamic
    --el0-lines strict --no-lift-lines "PMontt220->Chiloe110"
    --water-value-factor "$WATER_VALUE_FACTOR" --log-level INFO)
  [ -n "$lifts" ] && conv_args+=(--lift-line-caps "$lifts")
  t0=$(date +%s)
  ( cd "$SCRIPTS" && python3 -m plexos2gtopt.main "$datos" --output-dir "$out" \
      "${conv_args[@]}" ) > "$cdir/convert.log" 2>&1
  conv_rc=$?
  t1=$(date +%s); conv_s=$((t1-t0))
  json=$(ls "$out"/DATOS"$d8".json 2>/dev/null)
  if [ "$conv_rc" -ne 0 ] || [ -z "$json" ]; then
    echo "$date,$nlift,$conv_s,0,,,convert_failed," >> "$SUMMARY"
    echo "  CONVERT FAILED (rc=$conv_rc) — see $cdir/convert.log"; continue
  fi

  # 2) MIP solve: barrier (prm LPMethod=4) + crossover=auto + mip_start + 3h cap
  t0=$(date +%s)
  ( cd "$out" && timeout -k 120 "$SOLVE_TIMEOUT" "$GTOPT" "$(basename "$json")" \
      --solver cplex \
      --set solver_options.crossover=auto \
      --set monolithic_options.mip_start.enabled=true \
      --set solver_options.time_limit="$TIME_LIMIT" \
      --set model_options.loss_secant_segments=4 \
      -d output_mip ) > "$cdir/solve.log" 2>&1
  solve_rc=$?
  t1=$(date +%s); solve_s=$((t1-t0))

  # 3) harvest status + objective from solution.csv
  status="?"; obj=""
  sol="$out/output_mip/solution.csv"
  if [ -f "$sol" ]; then
    line=$(python3 -c "import csv;r=list(csv.DictReader(open('$sol')));print((r[0].get('status_name','?')+'\t'+str(r[0].get('obj_value',''))) if r else '?\t')" 2>/dev/null)
    status=$(printf '%s' "$line" | cut -f1); obj=$(printf '%s' "$line" | cut -f2)
  fi
  [ "$solve_rc" -eq 124 ] && status="TIMEOUT"
  echo "$date,$nlift,$conv_s,1,$solve_s,$solve_rc,$status,${obj}" >> "$SUMMARY"
  echo "  $date: lifts=$nlift convert=${conv_s}s solve=${solve_s}s rc=$solve_rc status=$status"

  # 4) cost compare vs PLEXOS (operational + FCF), partial table after each case
  res=$(ls "$casedir"/RES"$d8".zip.xz 2>/dev/null || ls "$casedir"/RES"$d8".zip 2>/dev/null)
  if [ -f "$sol" ] && [ -n "$res" ]; then
    ( cd "$SCRIPTS" && python3 -m plexos2gtopt.compare_with_plexos \
        --gtopt-output "$out/output_mip" --gtopt-bundle "$out" \
        --plexos-res-zip "$res" --no-uc-drilldown --width 150 ) \
        > "$cdir/compare.txt" 2>&1 && echo "  compared $date"
  fi
done
echo "==== MIP loop done ===="; cat "$SUMMARY"
