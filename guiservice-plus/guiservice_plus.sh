#!/usr/bin/env bash
# Launcher for the gtopt GUI Plus Next.js frontend.
#
# Environment variables:
#   GTOPT_GUISERVICE_URL   – Flask guiservice base URL (default: http://localhost:5000)
#   PORT                   – Port to listen on (default: 5002)
#
# Usage:
#   ./guiservice_plus.sh [dev|start]
#
# On first run, dependencies are installed with `npm install`.

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"

mode="${1:-start}"

if [[ ! -d node_modules ]]; then
  echo "Installing GUI Plus dependencies…"
  npm install --no-audit --no-fund
fi

export GTOPT_GUISERVICE_URL="${GTOPT_GUISERVICE_URL:-http://localhost:5000}"
export PORT="${PORT:-5002}"

echo "GUI Plus mode: $mode"
echo "Flask guiservice proxy: $GTOPT_GUISERVICE_URL"
echo "Listening on: http://localhost:$PORT"

case "$mode" in
  dev)
    npm run dev -- -p "$PORT"
    ;;
  build)
    npm run build
    ;;
  start)
    if [[ ! -d .next ]]; then
      echo "No production build found; running 'npm run build' first…"
      npm run build
    fi
    npm run start -- -p "$PORT"
    ;;
  *)
    echo "Unknown mode: $mode (expected: dev|build|start)" >&2
    exit 1
    ;;
esac
