#!/bin/bash
# gtopt_websrv - Launcher script for the gtopt web service
#
# This script is installed alongside the gtopt binary and provides
# a convenient way to launch the web service.

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
