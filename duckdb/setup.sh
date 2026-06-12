#!/usr/bin/env bash
# Create an isolated venv with DuckDB for the gtopt -> BI prototype.
# Kept separate from the gtopt Python tooling on purpose.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 -m venv "$here/.venv"
"$here/.venv/bin/pip" install --quiet --upgrade pip
"$here/.venv/bin/pip" install --quiet duckdb

echo "DuckDB ready: $("$here/.venv/bin/python" -c 'import duckdb; print(duckdb.__version__)')"
echo
echo "Build the semantic layer over a results dir:"
echo "  $here/.venv/bin/python $here/build_gtopt_db.py --results <results-dir> --db $here/gtopt.duckdb"
