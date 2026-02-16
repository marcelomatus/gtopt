#!/bin/bash
# Post-install test for the gtopt web service.
# Verifies that the web service starts and responds to HTTP requests
# after being installed via CMake. Can be run from either the source
# tree or the installed location.
#
# Usage:
#   ./test/install_test.sh [port]
#
# The optional port argument defaults to 3097.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEBSERVICE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
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

# ---- Resolve the gtopt binary ----
if [ -n "${GTOPT_BIN:-}" ] && [ -x "${GTOPT_BIN}" ]; then
  log "Using real gtopt binary: $GTOPT_BIN"
else
  log "GTOPT_BIN not set; creating mock binary for local testing"
  MOCK_BIN="$TEST_TMPDIR/mock_gtopt"
  cat > "$MOCK_BIN" << 'MOCK'
#!/bin/bash
echo "mock gtopt"
exit 0
MOCK
  chmod +x "$MOCK_BIN"
  GTOPT_BIN="$MOCK_BIN"
fi

# ---- Verify required files exist ----
if [ ! -d "$WEBSERVICE_DIR/node_modules" ]; then
  fail "node_modules not found in $WEBSERVICE_DIR (run npm install first)"
  exit 1
fi

if [ ! -d "$WEBSERVICE_DIR/.next" ]; then
  fail ".next not found in $WEBSERVICE_DIR (run npm run build first)"
  exit 1
fi

# ---- Ensure port is free ----
if curl -s "$BASE_URL" >/dev/null 2>&1; then
  fail "Port $PORT is already in use"
  exit 1
fi

# ---- Start the web service ----
log "Starting web service on port $PORT from $WEBSERVICE_DIR ..."
cd "$WEBSERVICE_DIR"
GTOPT_BIN="$GTOPT_BIN" GTOPT_DATA_DIR="$TEST_TMPDIR/data" \
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
pass "Server started successfully"

# ---- Test 1: GET / — landing page ----
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE_URL/")
if [ "$HTTP_CODE" = "200" ]; then
  pass "GET / returns 200"
else
  fail "GET / returned $HTTP_CODE (expected 200)"
fi

# ---- Test 2: GET /api/jobs — API responds ----
BODY=$(curl -s "$BASE_URL/api/jobs")
if echo "$BODY" | python3 -c "import sys,json; d=json.load(sys.stdin); assert isinstance(d['jobs'], list)" 2>/dev/null; then
  pass "GET /api/jobs returns a valid response"
else
  fail "GET /api/jobs unexpected response: $BODY"
fi

# ---- Summary ----
echo ""
echo "==============================="
echo "  Install test: $PASS passed, $FAIL failed"
echo "==============================="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
