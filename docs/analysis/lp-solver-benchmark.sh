#!/usr/bin/env bash
# =============================================================================
# LP Solver Benchmark Script
# Benchmarks CPLEX and HiGHS on a given .lp file with various option combos.
#
# Usage:
#   bash docs/analysis/lp-solver-benchmark.sh [LP_FILE]
#
# Defaults to support/lps/feasible_scene_1_phase_14.lp
# Requires: /opt/cplex/bin/x86-64_linux/cplex and/or highs on PATH
# =============================================================================

set -euo pipefail

LP="${1:-support/lps/feasible_scene_1_phase_14.lp}"
CPLEX="/opt/cplex/bin/x86-64_linux/cplex"
HIGHS="$(command -v highs 2>/dev/null || echo '')"
RUNS=7

echo "# LP Solver Benchmark — $(date -I)"
echo "# File: $LP ($(wc -l < "$LP") lines, $(du -h "$LP" | cut -f1))"
echo ""

# ---------------------------------------------------------------------------
# CPLEX benchmark
# ---------------------------------------------------------------------------
run_cplex() {
  local label="$1"
  shift
  local extra_cmds=("$@")
  echo "=== CPLEX: $label ==="
  for run in $(seq 1 "$RUNS"); do
    local start end elapsed_ms ticks result
    start=$(date +%s%N)
    result=$($CPLEX -c \
      "set threads 4" \
      "set lpmethod 4" \
      "set read scale 0" \
      "${extra_cmds[@]}" \
      "read $LP" \
      "optimize" \
      "quit" 2>&1)
    end=$(date +%s%N)
    elapsed_ms=$(( (end - start) / 1000000 ))
    ticks=$(echo "$result" | grep -o "Deterministic time = [0-9.]*" | head -1 | awk '{print $NF}')
    obj=$(echo "$result" | grep -oP "Objective =\s+\S+" | head -1)
    echo "  run$run: ${elapsed_ms}ms | ${ticks} ticks | $obj"
  done
}

if [[ -x "$CPLEX" ]]; then
  echo "## CPLEX Benchmark"
  echo ""

  # Baseline
  run_cplex "BASELINE (barrier/default_scaling/4t)"

  # Crossover variants
  run_cplex "crossover=0 (disabled)" "set barrier crossover 0"
  run_cplex "crossover=1 (primal)" "set barrier crossover 1"
  run_cplex "crossover=2 (dual)" "set barrier crossover 2"

  # Ordering variants
  run_cplex "ordering=1 (AMD)" "set barrier ordering 1"
  run_cplex "ordering=2 (AMF)" "set barrier ordering 2"
  run_cplex "ordering=3 (NestedDissection)" "set barrier ordering 3"

  # Parallel mode
  run_cplex "parallel=-1 (opportunistic)" "set parallel -1"

  # Convergence tolerance
  run_cplex "convergetol=1e-7" "set barrier convergetol 1e-7"

  # Preprocessing
  run_cplex "aggregator=0" "set preprocessing aggregator 0"

  # Corrections
  run_cplex "corrections=0" "set barrier limits corrections 0"

  # Numerical emphasis
  run_cplex "numerical emphasis=y" "set emphasis numerical y"

  # Best combos
  run_cplex "COMBO: parallel=-1 + crossover=1" \
    "set parallel -1" "set barrier crossover 1"
  run_cplex "COMBO: crossover=1 + ordering=1" \
    "set barrier crossover 1" "set barrier ordering 1"
  run_cplex "COMBO: parallel=-1 + crossover=1 + ordering=1" \
    "set parallel -1" "set barrier crossover 1" "set barrier ordering 1"

  # Thread sweep with crossover=1
  for t in 0 1 2 3 4; do
    run_cplex "crossover=1, threads=$t" "set barrier crossover 1" "set threads $t"
  done

  echo ""
else
  echo "## CPLEX: not found at $CPLEX — skipping"
  echo ""
fi

# ---------------------------------------------------------------------------
# HiGHS benchmark
# ---------------------------------------------------------------------------
run_highs() {
  local label="$1"
  shift
  local extra_opts=("$@")
  echo "=== HiGHS: $label ==="
  for run in $(seq 1 "$RUNS"); do
    local start end elapsed_ms result
    start=$(date +%s%N)
    result=$($HIGHS --model_file "$LP" "${extra_opts[@]}" 2>&1)
    end=$(date +%s%N)
    elapsed_ms=$(( (end - start) / 1000000 ))
    obj=$(echo "$result" | grep -i "objective" | grep -oP "[-+]?[0-9]*\.?[0-9]+[eE]?[-+]?[0-9]*" | tail -1)
    status=$(echo "$result" | grep -i "status" | tail -1)
    echo "  run$run: ${elapsed_ms}ms | obj=$obj | $status"
  done
}

if [[ -n "$HIGHS" ]]; then
  echo "## HiGHS Benchmark"
  echo ""

  # Baseline: IPM (barrier)
  run_highs "BASELINE (ipm/4t)" --solver ipm --threads 4

  # Algorithm variants
  run_highs "simplex (dual)" --solver simplex --threads 4
  run_highs "choose (auto)" --solver choose --threads 4

  # Thread sweep with IPM
  for t in 0 1 2 3 4 8; do
    run_highs "ipm, threads=$t" --solver ipm --threads $t
  done

  # Presolve off
  run_highs "ipm, presolve=off" --solver ipm --threads 4 --presolve off

  echo ""
else
  echo "## HiGHS: not found on PATH — skipping"
  echo ""
fi

# ---------------------------------------------------------------------------
# MindOpt benchmark
# ---------------------------------------------------------------------------
MINDOPT_HOME="${MINDOPT_HOME:-$HOME/mindopt/2.3.0/linux64-x86}"
MINDOPT_BIN="$MINDOPT_HOME/bin/mindopt"

run_mindopt() {
  local label="$1"
  shift
  local params=("$@")
  echo "=== MindOpt: $label ==="
  for run in $(seq 1 "$RUNS"); do
    local start end elapsed_ms result
    start=$(date +%s%N)
    result=$(LD_LIBRARY_PATH="$MINDOPT_HOME/lib:${LD_LIBRARY_PATH:-}" \
      $MINDOPT_BIN "$LP" "${params[@]}" 2>&1) || true
    end=$(date +%s%N)
    elapsed_ms=$(( (end - start) / 1000000 ))
    obj=$(echo "$result" | grep -oP 'Objective\s+:\s+\K[\S]+' | tail -1)
    iters=$(echo "$result" | grep -oP 'Num\. iterations\s+:\s+\K[\d]+' || echo "N/A")
    status=$(echo "$result" | grep -oP 'Optimizer status\s+:\s+\K\S+' || echo "N/A")
    echo "  run$run: ${elapsed_ms}ms | iters=$iters | obj=$obj | $status"
  done
}

if [[ -x "$MINDOPT_BIN" ]]; then
  echo "## MindOpt Benchmark"
  echo ""

  # Method comparison
  run_mindopt "AUTO (Method=-1, 4t)" Method=-1 NumThreads=4
  run_mindopt "DUAL SIMPLEX (Method=1, 4t)" Method=1 NumThreads=4
  run_mindopt "PRIMAL SIMPLEX (Method=0, 4t)" Method=0 NumThreads=4
  run_mindopt "BARRIER (Method=2, 4t)" Method=2 NumThreads=4

  # Best variants
  run_mindopt "barrier, PostScaling=0" Method=2 NumThreads=4 PostScaling=0
  run_mindopt "barrier, IPM/GapTol=1e-10" Method=2 NumThreads=4 IPM/GapTolerance=1e-10

  # Thread sweep with barrier
  for t in 1 2 4 8; do
    run_mindopt "barrier, threads=$t" Method=2 NumThreads=$t
  done

  # Presolve
  run_mindopt "barrier, presolve=off" Method=2 NumThreads=4 Presolve=0

  # Dualization
  run_mindopt "barrier, dualization=on" Method=2 NumThreads=4 Dualization=1

  echo ""
else
  echo "## MindOpt: not found at $MINDOPT_BIN — skipping"
  echo ""
fi

echo "# Benchmark complete."
