#!/bin/bash
# End-to-end integration test for gtopt_gui + gtopt_websrv connectivity.
#
# Usage:
#   ./guiservice/tests/test_gtopt_gui_integration.sh <install-prefix> <repo-root>

set -euo pipefail

INSTALL_PREFIX="${1:?install prefix required}"
REPO_ROOT="${2:?repo root required}"
GUI_PORT="${GUI_PORT:-5101}"
WEBSERVICE_PORT="${WEBSERVICE_PORT:-3001}"
GUI_URL="http://localhost:${GUI_PORT}"
TMPDIR="$(mktemp -d)"
GUI_PID=""

cleanup() {
  if [ -n "${GUI_PID}" ]; then
    kill "${GUI_PID}" 2>/dev/null || true
    wait "${GUI_PID}" 2>/dev/null || true
  fi
  rm -rf "${TMPDIR}"
}
trap cleanup EXIT

if [ -z "${GTOPT_BIN:-}" ] || [ ! -x "${GTOPT_BIN}" ]; then
  MOCK_BIN_DIR="${TMPDIR}/mockbin"
  mkdir -p "${MOCK_BIN_DIR}"
  cat > "${MOCK_BIN_DIR}/gtopt" << 'MOCK'
#!/bin/bash
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
mkdir -p "$OUTPUT_DIR/Generator"
echo "objective_value,status,solver_time" > "$OUTPUT_DIR/solution.csv"
echo "42.0,optimal,0.01" >> "$OUTPUT_DIR/solution.csv"
echo "generator,stage,block,scenario,value" > "$OUTPUT_DIR/Generator/generation_sol.csv"
echo "g1,1,1,1,10.0" >> "$OUTPUT_DIR/Generator/generation_sol.csv"
exit 0
MOCK
  chmod +x "${MOCK_BIN_DIR}/gtopt"
  export PATH="${MOCK_BIN_DIR}:${PATH}"
fi

CASE_ZIP="${TMPDIR}/case_c0.zip"
(cd "${REPO_ROOT}/cases/c0" && zip -r "${CASE_ZIP}" system_c0.json system_c0/) >/dev/null

timeout 120 "${INSTALL_PREFIX}/bin/gtopt_gui" \
  --webservice-port "${WEBSERVICE_PORT}" \
  --no-browser \
  --port "${GUI_PORT}" > "${TMPDIR}/gtopt_gui.log" 2>&1 &
GUI_PID=$!

for _ in $(seq 1 60); do
  if curl -sf "${GUI_URL}/api/schemas" >/dev/null; then
    break
  fi
  sleep 1
done
curl -sf "${GUI_URL}/api/schemas" >/dev/null

for _ in $(seq 1 30); do
  if curl -sf "${GUI_URL}/api/solve/ping" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('status') == 'ok'" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
curl -sf "${GUI_URL}/api/solve/ping" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('status') == 'ok'"
curl -sf "${GUI_URL}/api/solve/logs?lines=20" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'lines' in d"

curl -sf -X POST "${GUI_URL}/api/case/upload" -F "file=@${CASE_ZIP}" > "${TMPDIR}/case.json"
curl -sf -X POST "${GUI_URL}/api/solve/submit" \
  -H "Content-Type: application/json" \
  --data-binary @"${TMPDIR}/case.json" > "${TMPDIR}/submit.json"
TOKEN="$(python3 -c "import json; print(json.load(open('${TMPDIR}/submit.json'))['token'])")"

STATUS=""
for _ in $(seq 1 120); do
  STATUS="$(curl -sf "${GUI_URL}/api/solve/status/${TOKEN}" | python3 -c "import sys,json; print(json.load(sys.stdin)['status'])")"
  if [ "${STATUS}" = "completed" ] || [ "${STATUS}" = "failed" ]; then
    break
  fi
  sleep 1
done

if [ "${STATUS}" != "completed" ]; then
  cat "${TMPDIR}/gtopt_gui.log" || true
  echo "Solve job did not complete successfully (status=${STATUS})" >&2
  exit 1
fi

curl -sf "${GUI_URL}/api/solve/results/${TOKEN}" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'solution' in d"

echo "✓ gtopt_gui integrated connectivity and solve test passed"
