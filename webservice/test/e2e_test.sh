#!/bin/bash
# End-to-end test for the gtopt web service using the real gtopt binary.
# This script starts the web service pointed at an actual gtopt build,
# submits the cases/c0 case, downloads the results, and validates output
# against the expected reference files in cases/c0/output/.
#
# Usage:
#   ./test/e2e_test.sh [port]
#
# Environment:
#   GTOPT_BIN  — path to the real gtopt binary (required)
#
# The optional port argument defaults to 3098.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEBSERVICE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$WEBSERVICE_DIR/.." && pwd)"
PORT="${1:-3098}"
BASE_URL="http://localhost:$PORT"
TEST_TMPDIR=$(mktemp -d)
PASS=0
FAIL=0
SERVER_PID=""
TOLERANCE="1e-6"

cleanup() {
  if [ -n "$SERVER_PID" ]; then
    kill "$SERVER_PID" 2>/dev/null || true
    for child in $(pgrep -P "$SERVER_PID" 2>/dev/null); do
      kill "$child" 2>/dev/null || true
    done
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TEST_TMPDIR"
}
trap cleanup EXIT

log()  { echo "  [INFO] $*"; }
pass() { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }

# ---- Resolve the real gtopt binary ----
if [ -z "${GTOPT_BIN:-}" ]; then
  # Try common build locations
  for candidate in \
    "$REPO_DIR/build/standalone/gtopt" \
    "$REPO_DIR/build/gtopt" \
    "$(command -v gtopt 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
      GTOPT_BIN="$candidate"
      break
    fi
  done
fi

if [ -z "${GTOPT_BIN:-}" ] || [ ! -x "${GTOPT_BIN}" ]; then
  echo "ERROR: gtopt binary not found. Set GTOPT_BIN or build the project first." >&2
  exit 1
fi
log "Using gtopt binary: $GTOPT_BIN"

# ---- Validate test case exists ----
CASE_DIR="$REPO_DIR/cases/c0"
EXPECTED_DIR="$CASE_DIR/output"
if [ ! -f "$CASE_DIR/system_c0.json" ]; then
  echo "ERROR: Test case not found at $CASE_DIR/system_c0.json" >&2
  exit 1
fi

# ---- Create test zip from cases/c0 ----
TEST_ZIP="$TEST_TMPDIR/case_c0.zip"
if ! (cd "$CASE_DIR" && zip -r "$TEST_ZIP" system_c0.json system_c0/) >/dev/null; then
  echo "ERROR: Failed to create test zip from $CASE_DIR" >&2
  exit 1
fi

# ---- Ensure port is free ----
if curl -s "http://localhost:$PORT" >/dev/null 2>&1; then
  fail "Port $PORT is already in use"
  exit 1
fi

# ---- Start the web service with the real gtopt binary ----
log "Starting web service on port $PORT ..."
cd "$WEBSERVICE_DIR"
LOG_DIR="$TEST_TMPDIR/logs"
mkdir -p "$LOG_DIR"
GTOPT_BIN="$GTOPT_BIN" GTOPT_DATA_DIR="$TEST_TMPDIR/data" GTOPT_LOG_DIR="$LOG_DIR" \
  node_modules/.bin/next start -p "$PORT" \
  >"$TEST_TMPDIR/server.log" 2>&1 &
SERVER_PID=$!

for i in $(seq 1 30); do
  if curl -s "$BASE_URL" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

if ! curl -s "$BASE_URL" >/dev/null 2>&1; then
  fail "Server did not start within 30 seconds"
  cat "$TEST_TMPDIR/server.log"
  exit 1
fi
pass "Server started with real gtopt binary"

# ---- Submit the c0 case ----
log "Submitting case c0 ..."
SUBMIT=$(curl -s -X POST "$BASE_URL/api/jobs" \
  -F "file=@$TEST_ZIP" \
  -F "systemFile=system_c0.json")
TOKEN=$(echo "$SUBMIT" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null || true)
if [ -n "$TOKEN" ]; then
  pass "POST /api/jobs returned token: $TOKEN"
else
  fail "POST /api/jobs did not return a token: $SUBMIT"
  exit 1
fi

# ---- Wait for job to complete (real solver may take longer) ----
log "Waiting for job to complete ..."
STATUS=""
for i in $(seq 1 120); do
  BODY=$(curl -s "$BASE_URL/api/jobs/$TOKEN")
  STATUS=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin)['status'])" 2>/dev/null || true)
  if [ "$STATUS" = "completed" ] || [ "$STATUS" = "failed" ]; then
    break
  fi
  sleep 1
done

if [ "$STATUS" = "completed" ]; then
  pass "Job completed successfully"
elif [ "$STATUS" = "failed" ]; then
  ERROR=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('error','unknown'))" 2>/dev/null || true)
  fail "Job failed: $ERROR"
  # Show server logs for debugging
  log "Server log:"
  cat "$TEST_TMPDIR/server.log" 2>/dev/null || true
  # Try to show stdout/stderr from the job
  RESULT_ZIP="$TEST_TMPDIR/results.zip"
  curl -s -o "$RESULT_ZIP" "$BASE_URL/api/jobs/$TOKEN/download" 2>/dev/null || true
  mkdir -p "$TEST_TMPDIR/debug"
  unzip -o "$RESULT_ZIP" -d "$TEST_TMPDIR/debug" >/dev/null 2>&1 || true
  if [ -f "$TEST_TMPDIR/debug/stdout.log" ]; then
    log "Job stdout:"; cat "$TEST_TMPDIR/debug/stdout.log"
  fi
  if [ -f "$TEST_TMPDIR/debug/stderr.log" ]; then
    log "Job stderr:"; cat "$TEST_TMPDIR/debug/stderr.log"
  fi
  exit 1
else
  fail "Job did not finish within 120s (status: $STATUS)"
  exit 1
fi

# ---- Download results ----
RESULT_ZIP="$TEST_TMPDIR/results.zip"
HTTP_CODE=$(curl -s -o "$RESULT_ZIP" -w "%{http_code}" "$BASE_URL/api/jobs/$TOKEN/download")
if [ "$HTTP_CODE" = "200" ]; then
  pass "Downloaded results zip"
else
  fail "Download returned HTTP $HTTP_CODE"
  exit 1
fi

EXTRACT_DIR="$TEST_TMPDIR/extracted"
mkdir -p "$EXTRACT_DIR"
if unzip -o "$RESULT_ZIP" -d "$EXTRACT_DIR" >/dev/null; then
  pass "Results zip is valid"
else
  fail "Results zip could not be extracted"
  exit 1
fi

OUTPUT_DIR="$EXTRACT_DIR/output"
if [ ! -d "$OUTPUT_DIR" ]; then
  fail "No output/ directory in results"
  exit 1
fi

# ---- Validate solution.csv ----
SOLUTION_FILE="$OUTPUT_DIR/solution.csv"
if [ ! -f "$SOLUTION_FILE" ]; then
  fail "solution.csv not found in output"
else
  pass "solution.csv found"

  # Check solution has obj_value and status=0 (optimal)
  OBJ_VALUE=$(grep "^[[:space:]]*obj_value," "$SOLUTION_FILE" | cut -d',' -f2 | tr -d ' ' || true)
  SOL_STATUS=$(grep "^[[:space:]]*status," "$SOLUTION_FILE" | cut -d',' -f2 | tr -d ' ' || true)

  if [ -n "$OBJ_VALUE" ]; then
    pass "solution.csv contains obj_value: $OBJ_VALUE"
  else
    fail "solution.csv missing obj_value"
  fi

  if [ "$SOL_STATUS" = "0" ]; then
    pass "solution status is optimal (0)"
  else
    fail "solution status is not optimal: '$SOL_STATUS'"
  fi
fi

# ---- Compare output CSV files against expected ----
# This mirrors the cmake compare_csv.cmake logic
compare_csv() {
  local actual="$1"
  local expected="$2"
  local rel_path="$3"

  if [ ! -f "$actual" ]; then
    fail "Missing output file: $rel_path"
    return
  fi

  local actual_lines expected_lines
  actual_lines=$(wc -l < "$actual")
  expected_lines=$(wc -l < "$expected")

  if [ "$actual_lines" -ne "$expected_lines" ]; then
    fail "$rel_path: line count mismatch (actual=$actual_lines, expected=$expected_lines)"
    return
  fi

  # Compare line by line using python for numeric tolerance
  if python3 - "$actual" "$expected" "$TOLERANCE" << 'PYEOF'
import sys, csv, math

actual_path, expected_path, tol_str = sys.argv[1], sys.argv[2], sys.argv[3]
tol = float(tol_str)
errors = []

with open(actual_path) as af, open(expected_path) as ef:
    for i, (al, el) in enumerate(zip(af, ef), 1):
        al, el = al.strip(), el.strip()
        if al == el:
            continue
        # Split by comma and compare fields
        a_fields = [f.strip() for f in al.split(',')]
        e_fields = [f.strip() for f in el.split(',')]
        if len(a_fields) != len(e_fields):
            errors.append(f"line {i}: field count mismatch")
            continue
        for j, (av, ev) in enumerate(zip(a_fields, e_fields)):
            if av == ev:
                continue
            try:
                af_val, ef_val = float(av), float(ev)
                if not math.isclose(af_val, ef_val, rel_tol=tol, abs_tol=tol):
                    errors.append(f"line {i} col {j+1}: {av} != {ev} (beyond tolerance)")
            except ValueError:
                errors.append(f"line {i} col {j+1}: '{av}' != '{ev}'")

if errors:
    for e in errors:
        print(f"  DIFF: {e}", file=sys.stderr)
    sys.exit(1)
PYEOF
  then
    pass "$rel_path matches expected"
  else
    fail "$rel_path differs from expected"
  fi
}

# Find all expected CSV files and compare
log "Comparing output against expected results ..."
EXPECTED_CSV_COUNT=0
while IFS= read -r -d '' csv_file; do
  rel_path="${csv_file#$EXPECTED_DIR/}"
  actual_file="$OUTPUT_DIR/$rel_path"
  compare_csv "$actual_file" "$csv_file" "$rel_path"
  EXPECTED_CSV_COUNT=$((EXPECTED_CSV_COUNT + 1))
done < <(find "$EXPECTED_DIR" -name "*.csv" -print0 | sort -z)

if [ "$EXPECTED_CSV_COUNT" -eq 0 ]; then
  fail "No expected CSV files found in $EXPECTED_DIR"
else
  pass "Compared $EXPECTED_CSV_COUNT expected CSV files"
fi

# ---- Dump logs for analysis ----
LOG_FILE="$LOG_DIR/gtopt-webservice.log"
log "--- Webservice log file ($LOG_FILE) ---"
tail -40 "$LOG_FILE" 2>/dev/null || log "(no log file found)"
log "--- End of logs ---"

# ---- Summary ----
echo ""
echo "==============================="
echo "  E2E Results: $PASS passed, $FAIL failed"
echo "==============================="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
