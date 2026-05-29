#!/usr/bin/env bash
# Install + initialize Apache Superset in its own venv and launch it against
# a gtopt DuckDB file built by build_gtopt_db.py.
#
# Usage:
#   bash duckdb/superset_quickstart.sh [path/to/gtopt.duckdb]
#
# The venv lives at ~/superset-venv (outside the repo and the gtopt py env).
# Re-running is safe: install/init steps are skipped if already done.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB="${1:-$here/gtopt.duckdb}"
DB="$(cd "$(dirname "$DB")" && pwd)/$(basename "$DB")"   # absolutize
VENV="${SUPERSET_VENV:-$HOME/superset-venv}"
PORT="${SUPERSET_PORT:-8088}"

if [[ ! -f "$DB" ]]; then
  echo "ERROR: DuckDB file not found: $DB" >&2
  echo "Build it first:" >&2
  echo "  $here/.venv/bin/python $here/build_gtopt_db.py --materialize" >&2
  exit 1
fi

# 1) venv + Superset + DuckDB dialect ---------------------------------------
if [[ ! -d "$VENV" ]]; then
  echo ">> creating venv at $VENV"
  python3 -m venv "$VENV"
fi
# shellcheck disable=SC1091
source "$VENV/bin/activate"
if ! python -c "import superset" 2>/dev/null; then
  echo ">> installing apache-superset + duckdb-engine (this takes a while)"
  pip install --quiet --upgrade pip
  pip install --quiet apache-superset duckdb-engine
fi
# Superset 6.1.0 still references np.product (removed in numpy 2) and pins
# pandas to <2.2 in its setup.py but the pip resolver does not enforce that
# against transitive upgrades.  Re-pin defensively on every launch -- cheap
# no-op when already at the right versions.
pip install --quiet 'numpy<2' 'pandas>=2.1.4,<2.2' 2>/dev/null || true

# duckdb in the Superset venv must be >= the version that wrote the file.
python - "$DB" <<'PY'
import duckdb, sys
con = duckdb.connect(sys.argv[1], read_only=True)
con.close()
print(f">> duckdb {duckdb.__version__} can open the file (read-only OK)")
PY

# 2) initialize (idempotent) ------------------------------------------------
export FLASK_APP=superset
# Persistent SECRET_KEY in a config file: a random env var on every launch
# rotates the key, which makes encrypted metadata-DB fields undecryptable on
# the next start and crashes the SPA bootstrap (get_spa_payload).
CONFIG_DIR="$HOME/.superset"
CONFIG_FILE="$CONFIG_DIR/superset_config.py"
mkdir -p "$CONFIG_DIR"
if [[ ! -f "$CONFIG_FILE" ]]; then
  echo ">> writing persistent SECRET_KEY to $CONFIG_FILE"
  printf 'SECRET_KEY = "%s"\n' "$(openssl rand -base64 42)" > "$CONFIG_FILE"
  chmod 600 "$CONFIG_FILE"
fi
export SUPERSET_CONFIG_PATH="$CONFIG_FILE"

echo ">> superset db upgrade"
superset db upgrade >/dev/null

if ! superset fab list-users 2>/dev/null | grep -q .; then
  echo ">> creating admin user (interactive)"
  superset fab create-admin
else
  echo ">> admin user already exists, skipping create-admin"
fi

echo ">> superset init"
superset init >/dev/null

# 3) hand off ---------------------------------------------------------------
URI="duckdb:///${DB}?access_mode=read_only"
cat <<EOF

==============================================================================
Superset is ready.  Launching on http://localhost:${PORT}

In the UI:
  1. Settings -> Database Connections -> + Database -> "Other"
  2. SQLAlchemy URI (read-only so it won't lock the file):
         ${URI}
  3. Datasets -> + Dataset -> schema "main" -> e.g. v_generator_generation_sol
  4. Chart it: time column = datetime, dimension = type, metric = SUM(value),
     filter scenario = 51
==============================================================================

EOF

# --reload is for Superset developers: Flask restarts on file changes, which
# rotates the SPA asset hashes mid-session and crashes long-lived browser tabs
# with React ChunkLoadError.  Omit it for stable user sessions.
exec superset run -p "${PORT}" --with-threads
