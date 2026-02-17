#!/bin/bash
# Integration test for the gtopt web service.
# When GTOPT_BIN is set, uses the real gtopt binary; otherwise creates a
# minimal mock so the test can still run locally without a full build.
#
# Usage:
#   ./test/integration_test.sh [port]
#
# Environment:
#   GTOPT_BIN  — path to the gtopt binary (optional; falls back to mock)
#
# The optional port argument defaults to 3099.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEBSERVICE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$WEBSERVICE_DIR/.." && pwd)"
PORT="${1:-3099}"
BASE_URL="http://localhost:$PORT"
TEST_TMPDIR=$(mktemp -d)
PASS=0
FAIL=0
SERVER_PID=""

cleanup() {
  if [ -n "$SERVER_PID" ]; then
    # Kill the server and all its child processes
    kill "$SERVER_PID" 2>/dev/null || true
    # Also kill any child processes (next-server spawned by next start)
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

# ---- Resolve the gtopt binary ----
if [ -n "${GTOPT_BIN:-}" ] && [ -x "${GTOPT_BIN}" ]; then
  log "Using real gtopt binary: $GTOPT_BIN"
  USING_REAL_BINARY=true
else
  log "GTOPT_BIN not set or not executable; creating mock binary for local testing"
  MOCK_BIN="$TEST_TMPDIR/mock_gtopt"
  cat > "$MOCK_BIN" << 'MOCK'
#!/bin/bash
# Handle --version flag
if [ "$1" = "--version" ]; then
  echo "gtopt mock 0.0.0 (test)"
  exit 0
fi
SYSTEM_FILE="" OUTPUT_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-directory) OUTPUT_DIR="$2"; shift 2 ;;
    *) [ -z "$SYSTEM_FILE" ] && SYSTEM_FILE="$1"; shift ;;
  esac
done
if [ ! -f "$SYSTEM_FILE" ]; then
  echo "ERROR: System file '$SYSTEM_FILE' not found" >&2; exit 1
fi
mkdir -p "$OUTPUT_DIR"
echo "objective_value,status,solver_time" >  "$OUTPUT_DIR/solution.csv"
echo "42.0,optimal,0.01"                 >> "$OUTPUT_DIR/solution.csv"
mkdir -p "$OUTPUT_DIR/Generator"
echo "generator,stage,block,scenario,value" >  "$OUTPUT_DIR/Generator/generation_sol.csv"
echo "g1,1,1,1,10.0"                       >> "$OUTPUT_DIR/Generator/generation_sol.csv"
exit 0
MOCK
  chmod +x "$MOCK_BIN"
  GTOPT_BIN="$MOCK_BIN"
  USING_REAL_BINARY=false
fi

# ---- Create test zip from cases/c0 ----
TEST_ZIP="$TEST_TMPDIR/case_c0.zip"
(cd "$REPO_DIR/cases/c0" && zip -r "$TEST_ZIP" system_c0.json system_c0/) >/dev/null 2>&1

# ---- Start the web service ----
# Ensure port is free
if curl -s "http://localhost:$PORT" >/dev/null 2>&1; then
  fail "Port $PORT is already in use"
  exit 1
fi

log "Starting web service on port $PORT ..."
cd "$WEBSERVICE_DIR"
LOG_DIR="$TEST_TMPDIR/logs"
mkdir -p "$LOG_DIR"
GTOPT_BIN="$GTOPT_BIN" GTOPT_DATA_DIR="$TEST_TMPDIR/data" GTOPT_LOG_DIR="$LOG_DIR" \
  node_modules/.bin/next start -p "$PORT" \
  >"$TEST_TMPDIR/server.log" 2>&1 &
SERVER_PID=$!

# Wait for the server to be ready
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
pass "Server started"

# ---- Test 1: GET / — landing page ----
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/")
if [ "$HTTP_CODE" = "200" ]; then
  pass "GET / returns 200"
else
  fail "GET / returned $HTTP_CODE (expected 200)"
fi

# ---- Test 1b: GET /api — API root health check ----
BODY=$(curl -s "$BASE_URL/api")
API_STATUS=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || true)
if [ "$API_STATUS" = "ok" ]; then
  pass "GET /api returns status ok"
else
  fail "GET /api unexpected response: $BODY"
fi

# ---- Test 2: GET /api/jobs — empty list ----
BODY=$(curl -s "$BASE_URL/api/jobs")
if echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); assert isinstance(d['jobs'], list)" 2>/dev/null; then
  pass "GET /api/jobs returns a jobs list"
else
  fail "GET /api/jobs unexpected response: $BODY"
fi

# ---- Test 3: POST /api/jobs — submit case ----
SUBMIT=$(curl -s -X POST "$BASE_URL/api/jobs" \
  -F "file=@$TEST_ZIP" \
  -F "systemFile=system_c0.json")
TOKEN=$(echo "$SUBMIT" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null || true)
if [ -n "$TOKEN" ]; then
  pass "POST /api/jobs returned token: $TOKEN"
else
  fail "POST /api/jobs did not return a token: $SUBMIT"
fi

# ---- Test 4: Wait for job to complete ----
# Real solver may take longer than mock
JOB_TIMEOUT=120
STATUS=""
for i in $(seq 1 $JOB_TIMEOUT); do
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
else
  fail "Job did not finish within ${JOB_TIMEOUT}s (status: $STATUS)"
fi

# ---- Test 5: GET /api/jobs/:token — status check ----
BODY=$(curl -s "$BASE_URL/api/jobs/$TOKEN")
GOT_STATUS=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin)['status'])" 2>/dev/null || true)
if [ "$GOT_STATUS" = "completed" ]; then
  pass "GET /api/jobs/:token returns completed status"
else
  fail "GET /api/jobs/:token unexpected status: $GOT_STATUS"
fi

# ---- Test 6: GET /api/jobs/:token/download — download results ----
RESULT_ZIP="$TEST_TMPDIR/results.zip"
HTTP_CODE=$(curl -s -o "$RESULT_ZIP" -w "%{http_code}" "$BASE_URL/api/jobs/$TOKEN/download")
if [ "$HTTP_CODE" = "200" ]; then
  pass "GET /api/jobs/:token/download returns 200"
else
  fail "GET /api/jobs/:token/download returned $HTTP_CODE"
fi

# ---- Test 7: Verify zip contents ----
EXTRACT_DIR="$TEST_TMPDIR/extracted"
mkdir -p "$EXTRACT_DIR"
if unzip -o "$RESULT_ZIP" -d "$EXTRACT_DIR" >/dev/null 2>&1; then
  pass "Downloaded zip is valid"
else
  fail "Downloaded zip could not be extracted"
fi

if [ -f "$EXTRACT_DIR/output/solution.csv" ]; then
  pass "solution.csv found in output"
else
  fail "solution.csv not found in output"
fi

if [ -f "$EXTRACT_DIR/output/Generator/generation_sol.csv" ]; then
  pass "Generator/generation_sol.csv found in output"
else
  fail "Generator/generation_sol.csv not found in output"
fi

if [ -f "$EXTRACT_DIR/job.json" ]; then
  pass "job.json found in download"
else
  fail "job.json not found in download"
fi

# ---- Test 8: Error handling — invalid token ----
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/api/jobs/nonexistent-token")
if [ "$HTTP_CODE" = "404" ]; then
  pass "GET /api/jobs/nonexistent-token returns 404"
else
  fail "GET /api/jobs/nonexistent-token returned $HTTP_CODE (expected 404)"
fi

# ---- Test 9: Error handling — missing file ----
BODY=$(curl -s -X POST "$BASE_URL/api/jobs" -F "systemFile=system_c0.json")
HTTP_CODE_CHECK=$(echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); print('ok' if 'error' in d else 'bad')" 2>/dev/null || true)
if [ "$HTTP_CODE_CHECK" = "ok" ]; then
  pass "POST /api/jobs without file returns error"
else
  fail "POST /api/jobs without file did not return error: $BODY"
fi

# ---- Test 10: GET /api/ping — health check ----
BODY=$(curl -s "$BASE_URL/api/ping")
PING_STATUS=$(echo "$BODY" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || true)
if [ "$PING_STATUS" = "ok" ]; then
  pass "GET /api/ping returns status ok"
else
  fail "GET /api/ping unexpected response: $BODY"
fi

# ---- Test 11: GET /api/logs — log retrieval ----
BODY=$(curl -s "$BASE_URL/api/logs?lines=10")
LOG_CHECK=$(echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); print('ok' if 'lines' in d else 'bad')" 2>/dev/null || true)
if [ "$LOG_CHECK" = "ok" ]; then
  pass "GET /api/logs returns log lines"
else
  fail "GET /api/logs unexpected response: $BODY"
fi

# ---- Test 12: --check-api via gtopt_websrv.js ----
if node "$WEBSERVICE_DIR/gtopt_websrv.js" --check-api --port "$PORT" >/dev/null 2>&1; then
  pass "gtopt_websrv.js --check-api succeeds against running server"
else
  fail "gtopt_websrv.js --check-api failed against running server"
fi

# ---- Test 13: Log file exists and contains expected entries ----
LOG_FILE="$LOG_DIR/gtopt-webservice.log"
if [ -f "$LOG_FILE" ]; then
  pass "Log file created at $LOG_FILE"
else
  fail "Log file not found at $LOG_FILE"
fi

# ---- Test 14: Log file contains startup environment info ----
if grep -q "\[startup\]" "$LOG_FILE" 2>/dev/null; then
  pass "Log file contains startup environment info"
else
  fail "Log file missing startup environment info"
fi

# ---- Test 15: Log file contains API request entries ----
if grep -q "GET /api/ping" "$LOG_FILE" 2>/dev/null; then
  pass "Log file contains API ping request log"
else
  fail "Log file missing API ping request log"
fi

# ---- Test 16: Log file contains job lifecycle entries ----
if grep -q "Job.*created" "$LOG_FILE" 2>/dev/null; then
  pass "Log file contains job creation log"
else
  fail "Log file missing job creation log"
fi

# ---- Test 17: Fetch logs via /api/logs endpoint and verify content ----
BODY=$(curl -s "$BASE_URL/api/logs?lines=50")
LOG_LINES=$(echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); print(len(d.get('lines',[])))" 2>/dev/null || echo "0")
if [ "$LOG_LINES" -gt 0 ] 2>/dev/null; then
  pass "GET /api/logs returns $LOG_LINES log lines with content"
else
  fail "GET /api/logs returned no log lines"
fi

# ---- Test 18: Dump server log and webservice log for analysis ----
log "--- Server stdout/stderr (server.log) ---"
tail -30 "$TEST_TMPDIR/server.log" 2>/dev/null || true
log "--- Webservice log file ($LOG_FILE) ---"
tail -30 "$LOG_FILE" 2>/dev/null || true
log "--- End of logs ---"
pass "Logs dumped for analysis"

# ---- Summary ----
echo ""
echo "==============================="
echo "  Results: $PASS passed, $FAIL failed"
echo "==============================="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
