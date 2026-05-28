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

# duckdb in the Superset venv must be >= the version that wrote the file.
python - "$DB" <<'PY'
import duckdb, sys
con = duckdb.connect(sys.argv[1], read_only=True)
con.close()
print(f">> duckdb {duckdb.__version__} can open the file (read-only OK)")
PY

# 2) initialize (idempotent) ------------------------------------------------
export FLASK_APP=superset
: "${SUPERSET_SECRET_KEY:=$(openssl rand -base64 42)}"
export SUPERSET_SECRET_KEY

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

exec superset run -p "${PORT}" --with-threads --reload
