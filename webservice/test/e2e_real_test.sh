#!/bin/bash
# Full-stack end-to-end test for the gtopt web service using ONLY real components.
# No mocks are used — this test requires a real gtopt binary and validates
# the complete workflow: API health, job submission, solver execution, result
# download, log retrieval, and output correctness.
#
# Usage:
#   ./test/e2e_real_test.sh [port]
#
# Environment:
#   GTOPT_BIN  — path to the real gtopt binary (auto-detected if not set)
#
# This test will SKIP (exit 0) if no real gtopt binary is available.
# It is intended for CI environments where gtopt has been built from source.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEBSERVICE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$WEBSERVICE_DIR/.." && pwd)"
PORT="${1:-3097}"
BASE_URL="http://localhost:$PORT"
TEST_TMPDIR=$(mktemp -d)
PASS=0
FAIL=0
SERVER_PID=""

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

# ---- Resolve the real gtopt binary (no mocks) ----
if [ -n "${GTOPT_BIN:-}" ] && [ -x "${GTOPT_BIN}" ]; then
  log "Using real gtopt binary: $GTOPT_BIN"
else
  GTOPT_CANDIDATES=(
    "$REPO_DIR/build/gtopt"
    "$REPO_DIR/build/standalone/gtopt"
    "$REPO_DIR/build/install/bin/gtopt"
  )
  GTOPT_BIN=""
  for candidate in "${GTOPT_CANDIDATES[@]}"; do
    if [ -x "$candidate" ]; then
      GTOPT_BIN="$candidate"
      break
    fi
  done

  if [ -z "$GTOPT_BIN" ] && command -v gtopt >/dev/null 2>&1; then
    GTOPT_BIN="$(command -v gtopt)"
  fi

  if [ -z "$GTOPT_BIN" ]; then
    log "SKIP: No real gtopt binary found. This test requires a real binary."
    log "Build gtopt first: cmake -Sstandalone -Bbuild && cmake --build build"
    echo ""
    echo "==============================="
    echo "  Full-stack E2E: SKIPPED (no real binary)"
    echo "==============================="
    exit 0
  fi
  log "Using real gtopt binary: $GTOPT_BIN"
fi

# Verify binary works
GTOPT_VERSION=$("$GTOPT_BIN" --version 2>&1 || true)
if [ -z "$GTOPT_VERSION" ]; then
  log "SKIP: gtopt binary at $GTOPT_BIN does not respond to --version"
  exit 0
fi
log "gtopt version: $GTOPT_VERSION"

# ---- Validate test case exists ----
CASE_DIR="$REPO_DIR/cases/c0"
EXPECTED_DIR="$CASE_DIR/output"
if [ ! -f "$CASE_DIR/system_c0.json" ]; then
  echo "ERROR: Test case not found at $CASE_DIR/system_c0.json" >&2
  exit 1
fi

# ---- Create test zip ----
TEST_ZIP="$TEST_TMPDIR/case_c0.zip"
(cd "$CASE_DIR" && zip -r "$TEST_ZIP" system_c0.json system_c0/) >/dev/null 2>&1

# ---- Ensure port is free ----
if curl -s "http://localhost:$PORT" >/dev/null 2>&1; then
  fail "Port $PORT is already in use"
  exit 1
fi

# ---- Start the web service with the real gtopt binary and logging ----
LOG_DIR="$TEST_TMPDIR/logs"
mkdir -p "$LOG_DIR"
log "Starting web service on port $PORT with real gtopt binary..."
cd "$WEBSERVICE_DIR"
GTOPT_BIN="$GTOPT_BIN" GTOPT_DATA_DIR="$TEST_TMPDIR/data" GTOPT_LOG_DIR="$LOG_DIR" \
  node_modules/.bin/next start -p "$PORT" --hostname 0.0.0.0 \
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

# ---- Test 1: GET /api — API root ----
BODY=$(curl -s "$BASE_URL/api")
API_STATUS=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || true)
if [ "$API_STATUS" = "ok" ]; then
  pass "GET /api returns status ok"
else
  fail "GET /api unexpected response: $BODY"
fi

# ---- Test 2: GET /api/ping — health check with real binary info ----
BODY=$(curl -s "$BASE_URL/api/ping")
PING_STATUS=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || true)
PING_SERVICE=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('service',''))" 2>/dev/null || true)
PING_BIN=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('gtopt_bin',''))" 2>/dev/null || true)
PING_VER=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('gtopt_version',''))" 2>/dev/null || true)
if [ "$PING_STATUS" = "ok" ] && [ "$PING_SERVICE" = "gtopt-webservice" ]; then
  pass "GET /api/ping returns status ok, service gtopt-webservice"
else
  fail "GET /api/ping unexpected: status=$PING_STATUS service=$PING_SERVICE"
fi
if [ -n "$PING_BIN" ]; then
  pass "GET /api/ping reports gtopt binary: $PING_BIN"
else
  fail "GET /api/ping did not report gtopt binary path"
fi
if [ -n "$PING_VER" ]; then
  pass "GET /api/ping reports gtopt version: $PING_VER"
else
  fail "GET /api/ping did not report gtopt version"
fi

# ---- Test 3: Submit a real case and run the solver ----
log "Submitting case c0 with real solver..."
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

# ---- Test 4: Wait for job to complete with real solver ----
log "Waiting for real solver to complete..."
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
  pass "Job completed successfully with real solver"
elif [ "$STATUS" = "failed" ]; then
  ERROR=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('error','unknown'))" 2>/dev/null || true)
  fail "Job failed with real solver: $ERROR"
else
  fail "Job did not finish within 120s (status: $STATUS)"
fi

# ---- Test 5: GET /api/jobs/:token/logs — check solver output ----
LOGS_BODY=$(curl -s "$BASE_URL/api/jobs/$TOKEN/logs")
STDOUT_LEN=$(echo "$LOGS_BODY" | python3 -c "import sys,json; print(len(json.load(sys.stdin).get('stdout','')))" 2>/dev/null || echo "0")
if [ "$STDOUT_LEN" -gt 0 ] 2>/dev/null; then
  pass "GET /api/jobs/:token/logs has solver stdout ($STDOUT_LEN bytes)"
else
  log "GET /api/jobs/:token/logs: no stdout captured (may be normal for some solvers)"
fi

# ---- Test 6: Download results ----
RESULT_ZIP="$TEST_TMPDIR/results.zip"
HTTP_CODE=$(curl -s -o "$RESULT_ZIP" -w "%{http_code}" "$BASE_URL/api/jobs/$TOKEN/download")
if [ "$HTTP_CODE" = "200" ]; then
  pass "GET /api/jobs/:token/download returns 200"
else
  fail "GET /api/jobs/:token/download returned $HTTP_CODE"
fi

# ---- Test 7: Extract and validate results ----
EXTRACT_DIR="$TEST_TMPDIR/extracted"
mkdir -p "$EXTRACT_DIR"
if unzip -o "$RESULT_ZIP" -d "$EXTRACT_DIR" >/dev/null 2>&1; then
  pass "Downloaded zip is valid"
else
  fail "Downloaded zip could not be extracted"
fi

OUTPUT_DIR="$EXTRACT_DIR/output"
if [ -f "$OUTPUT_DIR/solution.csv" ]; then
  pass "solution.csv found in output"
else
  fail "solution.csv not found in output"
fi

# ---- Test 8: Validate solution is optimal ----
SOL_STATUS=$(grep "^[[:space:]]*status," "$OUTPUT_DIR/solution.csv" 2>/dev/null | cut -d',' -f2 | tr -d ' ' || true)
if [ "$SOL_STATUS" = "0" ]; then
  pass "Solution status is optimal (0)"
else
  fail "Solution status is not optimal: '$SOL_STATUS'"
fi

# ---- Test 9: Compare output against expected reference files ----
TOLERANCE="1e-6"
compare_csv() {
  local actual="$1"
  local expected="$2"
  local rel_path="$3"

  if [ ! -f "$actual" ]; then
    fail "Missing output file: $rel_path"
    return
  fi

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

# ---- Test 10: GET /api/jobs — list includes our completed job ----
BODY=$(curl -s "$BASE_URL/api/jobs")
JOB_COUNT=$(echo "$BODY" | python3 -c "import sys,json; print(len(json.load(sys.stdin).get('jobs',[])))" 2>/dev/null || echo "0")
if [ "$JOB_COUNT" -gt 0 ] 2>/dev/null; then
  pass "GET /api/jobs lists $JOB_COUNT job(s)"
else
  fail "GET /api/jobs returned no jobs"
fi

# ---- Test 11: GET /api/logs — verify log file has entries ----
BODY=$(curl -s "$BASE_URL/api/logs?lines=100")
LOG_LINES=$(echo "$BODY" | python3 -c "import sys,json; print(len(json.load(sys.stdin).get('lines',[])))" 2>/dev/null || echo "0")
if [ "$LOG_LINES" -gt 0 ] 2>/dev/null; then
  pass "GET /api/logs returns $LOG_LINES log lines"
else
  fail "GET /api/logs returned no log lines"
fi

# ---- Test 12: Log file contains full job lifecycle ----
LOG_FILE="$LOG_DIR/gtopt-webservice.log"
if [ -f "$LOG_FILE" ]; then
  pass "Log file exists at $LOG_FILE"

  # Check for key lifecycle events
  for pattern in \
    "Job.*created" \
    "Job.*starting gtopt" \
    "Job.*gtopt completed successfully" \
    "Job.*saved stdout.log" \
    "GET /api/ping" \
    "startup"; do
    if grep -q "$pattern" "$LOG_FILE" 2>/dev/null; then
      pass "Log contains: $pattern"
    else
      fail "Log missing: $pattern"
    fi
  done
else
  fail "Log file not found at $LOG_FILE"
fi

# ---- Test 13: --check-api via gtopt_websrv.js ----
if node "$WEBSERVICE_DIR/gtopt_websrv.js" --check-api --port "$PORT" >/dev/null 2>&1; then
  pass "gtopt_websrv.js --check-api succeeds"
else
  fail "gtopt_websrv.js --check-api failed"
fi

# ---- Dump logs for analysis ----
log "--- Webservice log file (last 50 lines) ---"
tail -50 "$LOG_FILE" 2>/dev/null || log "(no log file found)"
log "--- End of logs ---"

# ---- Summary ----
echo ""
echo "==============================="
echo "  Full-stack E2E (real components): $PASS passed, $FAIL failed"
echo "==============================="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
