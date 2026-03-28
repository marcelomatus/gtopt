#!/bin/bash
# Integration test: run SDDP on gtopt_case_2y (first scenario)
#
# Converts plp_case_2y → gtopt format, then runs gtopt with SDDP for 1 iteration.
# Verifies that the solver finds an optimal (or at least non-infeasible) solution.
#
# Usage:
#   tools/test_sddp_case_2y.sh [gtopt_binary] [cases_dir]
#
# Exit codes:
#   0 = PASS (optimal solution found)
#   1 = FAIL-INFEASIBLE (non-optimal / infeasible — LP formulation bug)
#   2 = FAIL-OTHER (build error, option error, crash, etc.)

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GTOPT_BIN="${1:-gtopt}"
CASES_DIR="${2:-$REPO_DIR/scripts/cases}"

WORK_DIR=$(mktemp -d /tmp/gtopt_sddp_test_XXXXXX)
trap "rm -rf $WORK_DIR" EXIT

if ! command -v "$GTOPT_BIN" &>/dev/null && [ ! -x "$GTOPT_BIN" ]; then
    echo "ERROR: gtopt binary not found: $GTOPT_BIN"
    exit 2
fi

if ! command -v plp2gtopt &>/dev/null; then
    echo "ERROR: plp2gtopt not found on PATH"
    exit 2
fi

PLP_DIR="$CASES_DIR/plp_case_2y"
if [ ! -d "$PLP_DIR" ]; then
    echo "ERROR: plp_case_2y not found at $PLP_DIR"
    exit 2
fi

CASE_DIR="$WORK_DIR/gtopt_case_2y_fs"

echo "=== SDDP integration test: gtopt_case_2y ==="
echo "  gtopt binary : $GTOPT_BIN"
echo "  plp input    : $PLP_DIR"
echo "  work dir     : $WORK_DIR"

# Step 1: Convert PLP to GTOPT format (first scenario only)
echo "--- Step 1: plp2gtopt --first-scenario ---"
plp2gtopt --first-scenario -i "$PLP_DIR" -o "$CASE_DIR" 2>&1 | tail -5
if [ $? -ne 0 ]; then
    echo "ERROR: plp2gtopt failed"
    exit 2
fi

# Step 2: Run gtopt with SDDP, max 1 iteration
echo "--- Step 2: gtopt SDDP (1 iteration) ---"
OUTPUT="$WORK_DIR/output.log"

# Try with current CLI option names; fall back to older variants
run_gtopt() {
    local opts="$1"
    GTOPT_SOLVER=clp "$GTOPT_BIN" "$CASE_DIR" $opts >"$OUTPUT" 2>&1
}

run_gtopt "--sddp-max-iterations 1"

# If unknown option, try older name variants
if grep -qi "unknown.*option\|unrecognized" "$OUTPUT" 2>/dev/null; then
    echo "  (retrying with --sddp_max_iterations)"
    run_gtopt "--sddp_max_iterations 1"
fi

if grep -qi "unknown.*option\|unrecognized" "$OUTPUT" 2>/dev/null; then
    echo "  (retrying with --max-iterations)"
    run_gtopt "--max-iterations 1"
fi

if grep -qi "unknown.*option\|unrecognized" "$OUTPUT" 2>/dev/null; then
    echo "  (retrying with --max_iterations)"
    run_gtopt "--max_iterations 1"
fi

# Step 3: Analyze results
echo "--- Output (last 15 lines) ---"
tail -15 "$OUTPUT"

if grep -q "non-optimal" "$OUTPUT"; then
    echo ""
    echo "=== FAIL-INFEASIBLE: SDDP found non-optimal (infeasible) solution ==="
    # Show the infeasibility details
    grep -E "(non-optimal|elastic|infeas|failed|Error)" "$OUTPUT" | head -10
    exit 1
elif grep -q "Status.*:.*optimal" "$OUTPUT"; then
    echo ""
    echo "=== PASS: SDDP found optimal solution ==="
    exit 0
else
    echo ""
    echo "=== FAIL-OTHER: unexpected output ==="
    exit 2
fi
