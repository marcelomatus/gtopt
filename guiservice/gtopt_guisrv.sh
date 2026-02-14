#!/bin/bash
# gtopt_guisrv - Web server launcher for the gtopt GUI service
#
# This script starts the Flask guiservice as a web server (without opening
# a browser), similar to how gtopt_websrv runs the web service.

# Get the directory where this script is installed
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Find the guiservice directory
if [ -f "$SCRIPT_DIR/../share/gtopt/guiservice/gtopt_guisrv.py" ]; then
    GUISERVICE_DIR="$SCRIPT_DIR/../share/gtopt/guiservice"
elif [ -f "$SCRIPT_DIR/gtopt_guisrv.py" ]; then
    GUISERVICE_DIR="$SCRIPT_DIR"
else
    echo "Error: Cannot find guiservice directory" >&2
    exit 1
fi

# Find Python 3
PYTHON=""
for cmd in python3 python; do
    if command -v "$cmd" >/dev/null 2>&1; then
        # Check if it's Python 3.10+
        if "$cmd" -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" 2>/dev/null; then
            PYTHON="$cmd"
            break
        fi
    fi
done

if [ -z "$PYTHON" ]; then
    echo "Error: Python 3.10 or later is required" >&2
    echo "Please install Python 3.10+ and ensure it's in your PATH" >&2
    exit 1
fi

# Check if required Python packages are installed
if ! "$PYTHON" -c "import flask, pandas, pyarrow, requests" 2>/dev/null; then
    echo "Error: Required Python packages are not installed" >&2
    echo "" >&2
    echo "Please install the required packages:" >&2
    echo "  $PYTHON -m pip install -r $GUISERVICE_DIR/requirements.txt" >&2
    exit 1
fi

# Run the Python launcher
exec "$PYTHON" "$GUISERVICE_DIR/gtopt_guisrv.py" "$@"
