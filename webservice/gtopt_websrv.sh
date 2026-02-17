#!/bin/bash
# gtopt_websrv - Launcher script for the gtopt web service
#
# This script is installed alongside the gtopt binary and provides
# a convenient way to launch the web service.
#
# Usage:
#   gtopt_websrv [options]
#
# The --check-api flag verifies whether the API is responding on the
# given port (default 3000) and exits with 0 on success or 1 on failure.

# Get the directory where this script is installed
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Find the webservice directory
if [ -f "$SCRIPT_DIR/../share/gtopt/webservice/gtopt_websrv.js" ]; then
    WEBSERVICE_DIR="$SCRIPT_DIR/../share/gtopt/webservice"
elif [ -f "$SCRIPT_DIR/gtopt_websrv.js" ]; then
    WEBSERVICE_DIR="$SCRIPT_DIR"
else
    echo "Error: Cannot find webservice directory" >&2
    exit 1
fi

# ---- Helper functions for API verification ----

# Check a single API endpoint. Returns 0 on success.
#   gtopt_websrv_check_endpoint URL
gtopt_websrv_check_endpoint() {
    local url="$1"
    local body
    body=$(curl -s --max-time 5 "$url" 2>/dev/null) || return 1
    echo "$body" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
    if d.get('status') == 'ok':
        sys.exit(0)
    sys.exit(1)
except Exception:
    sys.exit(1)
" 2>/dev/null
}

# Verify the webservice API on a given port.  Checks /api and /api/ping.
# Tries both localhost and 127.0.0.1 to handle IPv6/IPv4 differences (WSL).
# Prints results and returns 0 when both checks pass, 1 otherwise.
#   gtopt_websrv_verify_api PORT [LOG_FILE]
gtopt_websrv_verify_api() {
    local port="${1:-3000}"
    local log_file="${2:-}"
    local base_urls=("http://127.0.0.1:${port}" "http://localhost:${port}")
    local ok=0

    _log_verify() {
        echo "$1"
        if [ -n "$log_file" ]; then
            echo "$1" >> "$log_file" 2>/dev/null || true
        fi
    }

    _log_verify "Verifying API endpoints on port ${port}..."

    local api_ok=false
    local working_url=""
    for base_url in "${base_urls[@]}"; do
        if gtopt_websrv_check_endpoint "${base_url}/api"; then
            _log_verify "API verification PASSED: GET ${base_url}/api returned status \"ok\""
            api_ok=true
            working_url="$base_url"
            break
        fi
    done
    if [ "$api_ok" = false ]; then
        _log_verify "API verification FAILED: GET /api did not respond on ${base_urls[*]}"
        ok=1
    fi

    local ping_url="${working_url:-${base_urls[0]}}"
    if gtopt_websrv_check_endpoint "${ping_url}/api/ping"; then
        local ping_body
        ping_body=$(curl -s --max-time 5 "${ping_url}/api/ping" 2>/dev/null)
        local service
        service=$(echo "$ping_body" | python3 -c "import sys,json; print(json.load(sys.stdin).get('service',''))" 2>/dev/null || true)
        local version
        version=$(echo "$ping_body" | python3 -c "import sys,json; print(json.load(sys.stdin).get('gtopt_version',''))" 2>/dev/null || true)
        _log_verify "API verification PASSED: GET /api/ping returned service=\"${service}\""
        [ -n "$version" ] && _log_verify "  gtopt version: ${version}"
    else
        _log_verify "API verification WARNING: GET /api/ping did not return expected response"
    fi

    _log_verify "API verification complete."
    return $ok
}

# ---- Main ----

# Find Node.js
NODE=""
for cmd in node nodejs; do
    if command -v "$cmd" >/dev/null 2>&1; then
        # Check if it's Node.js 18+
        VERSION=$("$cmd" --version 2>/dev/null | sed 's/v//' | cut -d. -f1)
        if [ -n "$VERSION" ] && [ "$VERSION" -ge 18 ]; then
            NODE="$cmd"
            break
        fi
    fi
done

if [ -z "$NODE" ]; then
    echo "Error: Node.js 18 or later is required" >&2
    echo "Please install Node.js from https://nodejs.org/" >&2
    exit 1
fi

# Run the Node.js launcher
exec "$NODE" "$WEBSERVICE_DIR/gtopt_websrv.js" "$@"
