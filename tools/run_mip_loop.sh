#!/usr/bin/env bash
# Serial MIP loop over all cached CEN PCP PLEXOS cases.
#
# K=10 loss segments (--nseg-losses 10), default config (auto-detected lifted
# lines, plexos horizon mode), one case at a time with a per-case watchdog so a
# hang doesn't block the rest.
#
# Calibrated terminal water values: the converter scales the per-reservoir
# boundary-cut slopes for the over-banking Biobío/Maule heads via
# --water-value-factor.  RALCO 0.80 / COLBUN 0.85 is the best-aggregate point
# found by the 2026-06-07 sweep (LP-relax operational cost -0.80% vs PLEXOS on
# 20251005, down from +8.94% at factor 1.0).  Override with the env var
# WATER_VALUE_FACTOR.
#
# Env overrides: GTOPT_BIN, BASE, WATER_VALUE_FACTOR, SOLVE_TIMEOUT, CACHE.
set -u

SCRIPTS="${SCRIPTS:-/home/marce/git/gtopt/scripts}"
GTOPT="${GTOPT_BIN:-$HOME/tmp/gtopt-build-fcfeq/standalone/gtopt}"
CACHE="${CACHE:-$HOME/.cache/gtopt/cen2gtopt/pcp_archive/PCP}"
BASE="${BASE:-$HOME/tmp/mip_loop_wvf}"
WATER_VALUE_FACTOR="${WATER_VALUE_FACTOR:-COLBUN:0.85,RALCO:0.80}"
SOLVE_TIMEOUT="${SOLVE_TIMEOUT:-3600}"   # per-case MIP watchdog (s)

mkdir -p "$BASE"
SUMMARY="$BASE/summary.csv"
echo "date,convert_s,convert_ok,solve_s,solve_rc,status,objective,rows,cols,int_vars" > "$SUMMARY"
echo "water-value-factor: $WATER_VALUE_FACTOR"
echo "binary: $GTOPT"
echo "base:   $BASE"

for zip in "$CACHE"/PLEXOS2*.zip; do
  date=$(basename "$zip" | sed -E 's/PLEXOS([0-9]+)\.zip/\1/')
  cdir=$BASE/$date
  src=$cdir/src; out=$cdir/out
  rm -rf "$cdir"; mkdir -p "$src"
  echo "================ $date ================"

  # 1) extract DATOS + RES as siblings (RES gives the t_phase_3 block layout)
  ( cd "$src" && unzip -o "$zip" >/dev/null 2>&1 )
  datos=$(ls "$src"/DATOS"$date".zip 2>/dev/null)
  if [ -z "$datos" ]; then echo "$date,,0,,,no_datos,,,," >> "$SUMMARY"; continue; fi

  # 2) convert with K=10 loss segments + calibrated water-value factors
  t0=$(date +%s)
  # Canonical CEN PCP loss/lift config (these match the plexos2gtopt
  # defaults, pinned here for reproducibility): nseg_losses=10,
  # nseg_tangent=6, nseg_uniform=4, dynamic PWL layout, EL=0 lines
  # strict, the curated 35-line lift set (default --lift-line-caps,
  # minus the PMontt->Chiloe cable via --no-lift-lines).  The MIP solve
  # below uses loss_secant_segments=4.
  ( cd "$SCRIPTS" && python3 -m plexos2gtopt.main "$datos" --output-dir "$out" \
      --nseg-losses 10 --nseg-tangent 6 --nseg-uniform 4 \
      --loss-error-pct 1.0 --loss-cost-eps 1.0 --loss-pwl-layout dynamic \
      --el0-lines strict --no-lift-lines PMontt220->Chiloe110 \
      --water-value-factor "$WATER_VALUE_FACTOR" \
      --log-level INFO ) > "$cdir/convert.log" 2>&1
  conv_rc=$?
  t1=$(date +%s); conv_s=$((t1-t0))
  json=$(ls "$out"/DATOS"$date".json 2>/dev/null)
  if [ $conv_rc -ne 0 ] || [ -z "$json" ]; then
    echo "$date,$conv_s,0,,,convert_failed,,,," >> "$SUMMARY"; continue
  fi

  # 3) MIP solve (default — no --no-mip)
  t0=$(date +%s)
  ( cd "$out" && timeout "$SOLVE_TIMEOUT" "$GTOPT" "$(basename "$json")" \
      --set model_options.loss_secant_segments=4 \
      -d output_mip ) > "$cdir/solve.log" 2>&1
  solve_rc=$?
  t1=$(date +%s); solve_s=$((t1-t0))

  # 4) harvest stats from solver_status.json + logs
  st="$out/output_mip/solver_status.json"
  status=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get('status','?'))" "$st" 2>/dev/null || echo "no_status")
  obj=$(grep -aoE "Objective[^=]*=\s*-?[0-9.eE+]+" "$cdir/solve.log" 2>/dev/null | tail -1 | grep -oE "\-?[0-9.eE+]+$")
  rc_line=$(grep -aoE "avg LP size\s*:\s*[0-9,]+ vars, [0-9,]+ rows" "$out/output_mip/logs/"gtopt_*.log 2>/dev/null | tail -1)
  cols=$(echo "$rc_line" | grep -oE "[0-9,]+ vars" | tr -d ', vars')
  rows=$(echo "$rc_line" | grep -oE "[0-9,]+ rows" | tr -d ', rows')
  intv=$(grep -aoE "Reduced MIP has [0-9]+ binaries, [0-9]+ generals" "$out/output_mip/logs/"cplex_*.log 2>/dev/null | tail -1 | grep -oE "[0-9]+" | paste -sd+ | bc 2>/dev/null)
  echo "$date,$conv_s,1,$solve_s,$solve_rc,$status,${obj:-},${rows:-},${cols:-},${intv:-}" >> "$SUMMARY"
  echo "  $date: convert ${conv_s}s, solve ${solve_s}s rc=$solve_rc status=$status obj=${obj:-?}"
done
echo "==== MIP loop done ===="; cat "$SUMMARY"
