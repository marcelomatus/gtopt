#!/bin/bash
# gtopt_gui - Launcher script for the gtopt GUI service
#
# This script is installed alongside the gtopt binary and provides
# a convenient way to launch the web-based GUI.

# Get the directory where this script is installed
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER_DEBUG="${GTOPT_GUI_DEBUG:-0}"

log_launcher() {
    if [ "$LAUNCHER_DEBUG" = "1" ]; then
        echo "[gtopt_gui.sh] $*" >&2
    fi
}

# Find the guiservice directory
# When installed, it should be in the same prefix under share/gtopt/guiservice
if [ -f "$SCRIPT_DIR/../share/gtopt/guiservice/gtopt_gui.py" ]; then
    GUISERVICE_DIR="$SCRIPT_DIR/../share/gtopt/guiservice"
elif [ -f "$SCRIPT_DIR/gtopt_gui.py" ]; then
    GUISERVICE_DIR="$SCRIPT_DIR"
else
    echo "Error: Cannot find guiservice directory" >&2
    exit 1
fi
log_launcher "SCRIPT_DIR=$SCRIPT_DIR"
log_launcher "GUISERVICE_DIR=$GUISERVICE_DIR"
log_launcher "PATH=$PATH"
log_launcher "GTOPT_GUI_PYTHON=${GTOPT_GUI_PYTHON:-<unset>}"
log_launcher "GTOPT_WEBSERVICE_URL=${GTOPT_WEBSERVICE_URL:-<unset>}"

# If guiservice files are not user-writable (e.g. installed under /usr/local),
# run from a user-owned temporary copy.
if [ ! -w "$GUISERVICE_DIR" ]; then
    TMP_GUISERVICE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/gtopt_gui_XXXXXX")"
    if [ -z "$TMP_GUISERVICE_DIR" ] || [ ! -d "$TMP_GUISERVICE_DIR" ]; then
        echo "Error: Failed to create temporary guiservice directory" >&2
        exit 1
    fi
    if ! cp -a "$GUISERVICE_DIR/." "$TMP_GUISERVICE_DIR/"; then
        echo "Error: Failed to copy guiservice files to temporary directory" >&2
        exit 1
    fi
    GUISERVICE_DIR="$TMP_GUISERVICE_DIR"
    log_launcher "Using temporary GUISERVICE_DIR=$GUISERVICE_DIR"
fi

# Find Python 3
PYTHON="${GTOPT_GUI_PYTHON:-}"
if [ -n "$PYTHON" ]; then
    log_launcher "Trying GTOPT_GUI_PYTHON override: $PYTHON"
    if ! command -v "$PYTHON" >/dev/null 2>&1; then
        echo "Error: GTOPT_GUI_PYTHON is set but not executable on PATH: $PYTHON" >&2
        exit 1
    fi
    if ! "$PYTHON" -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" 2>/dev/null; then
        echo "Error: GTOPT_GUI_PYTHON must point to Python 3.10 or later: $PYTHON" >&2
        exit 1
    fi
else
    for cmd in python3 python; do
        if command -v "$cmd" >/dev/null 2>&1; then
            # Check if it's Python 3
            if "$cmd" -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)" 2>/dev/null; then
                PYTHON="$cmd"
                break
            fi
        fi
    done
fi

if [ -z "$PYTHON" ]; then
    echo "Error: Python 3.10 or later is required" >&2
    echo "Please install Python 3.10+ and ensure it's in your PATH" >&2
    exit 1
fi
log_launcher "Using PYTHON=$PYTHON"

# Check if required Python packages are installed
if ! "$PYTHON" -c "import flask, pandas, pyarrow, requests" 2>/dev/null; then
    echo "Error: Required Python packages are not installed" >&2
    echo "" >&2
    echo "Please install the required packages:" >&2
    echo "  $PYTHON -m pip install -r $GUISERVICE_DIR/requirements.txt" >&2
    echo "" >&2
    echo "Or create a virtual environment:" >&2
    echo "  $PYTHON -m venv ~/.gtopt-gui-venv" >&2
    echo "  source ~/.gtopt-gui-venv/bin/activate" >&2
    echo "  pip install -r $GUISERVICE_DIR/requirements.txt" >&2
    exit 1
fi

# Run the Python launcher
log_launcher "Executing: $PYTHON $GUISERVICE_DIR/gtopt_gui.py $*"
exec "$PYTHON" "$GUISERVICE_DIR/gtopt_gui.py" "$@"
