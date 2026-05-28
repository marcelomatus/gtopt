# gtopt → DuckDB semantic layer (BI prototype)

A thin DuckDB layer that makes gtopt's Parquet solution directly usable by
open-source BI tools (**Apache Superset**, Metabase, Grafana). It builds
**views only** — the original Parquet files are never copied or rewritten.

## Why a layer is needed

gtopt writes each stream as a Hive-partitioned Parquet dataset:

```
<results>/<Collection>/<field>_<stream>.parquet/scene=N/phase=M/part.parquet
```

gtopt may write each stream in either of two shapes (this layer handles both):

* **Wide** (legacy): `scenario, stage, block` + one `uid:<N>` column per
  element (≈990 columns for generators). Needs `UNPIVOT` to be long-tidy.
* **Long** (current, observed on May 2026 runs): `scenario, stage, block,
  uid, value` already long-format. Used directly — no UNPIVOT.

In both cases, the same trap applies:

⚠️ **`scene`/`phase` are a NULL-padded grid, not data partitions.** Each
`(scene, phase)` partition writes the full `(scenario, stage, block[, uid])`
grid but fills only its own diagonal cell. For the 16-scene/20-phase juan
run that is **320 physical rows per model point**, 319 of them NULL. On the
non-null cells, `scene` is a 1:1 relabel of `scenario` and `phase` of `stage`
(verified: 2,044,208 non-null values == distinct
`(scenario, stage, block, uid)`). A naive `SUM(value)` straight off the raw
files inflates results 320×.

The layer removes the trap two ways: NULLs are dropped (`UNPIVOT` for wide,
`WHERE value IS NOT NULL` for long), and the user-facing `v_*` views drop
`scene`/`phase` entirely, keeping `scenario / stage / block / uid` as the
canonical model index.

## What it builds

| Object | What it is |
|--------|-----------|
| `dim_<entity>` | Dimension tables decoded from `planning.json` (`dim_generator`, `dim_bus`, `dim_line`, `dim_reservoir`, …) — uid → name, type, bus, … |
| `dim_stage`, `dim_block`, `dim_scenario`, `dim_time` | Calendar: stage `month`, block `duration`, cumulative `hours_from_start`, and a synthetic `datetime` |
| `fact_<coll>_<stream>` | Raw long view: wide→long via `UNPIVOT`, `uid:<N>` folded to integer `uid`. Keeps `scene`/`phase` for traceability |
| `v_<coll>_<stream>` | **Use these.** `fact_` joined to its dimension (names/attrs) and the calendar; `scene`/`phase` dropped |
| `meta_streams` | Catalog of every discovered stream (collection, field, kind, dimension) |

## Setup & build

```bash
bash duckdb/setup.sh                       # isolated venv + duckdb
duckdb/.venv/bin/python duckdb/build_gtopt_db.py \
    --results support/juan/results \
    --db duckdb/gtopt.duckdb
```

`--epoch '2020-01-01 00:00:00'` sets the synthetic datetime anchor (the gtopt
calendar has month labels + block durations but no absolute start instant).

## Query examples

```sql
-- What streams exist?
FROM meta_streams;

-- Generation dispatch, one operating point (names resolved, no padding):
SELECT name, type, round(value,1) AS mw
FROM v_generator_generation_sol
WHERE scenario=51 AND stage=1 AND block=1 AND value>0
ORDER BY mw DESC LIMIT 10;

-- Generation by fuel type over time (the calendar gives a real datetime axis):
SELECT datetime, type, sum(value) AS mw
FROM v_generator_generation_sol
WHERE scenario=51
GROUP BY datetime, type ORDER BY datetime;

-- Bus marginal prices (LMP) from the balance dual:
SELECT name AS bus, datetime, value AS lmp
FROM v_bus_balance_dual
WHERE scenario=51 ORDER BY datetime;
```

## Connecting Apache Superset

Superset talks to DuckDB through the `duckdb-engine` SQLAlchemy dialect:

```bash
pip install duckdb-engine        # in the Superset environment
```

Add a database in Superset with SQLAlchemy URI:

```
duckdb:////absolute/path/to/duckdb/gtopt.duckdb
```

Then register the `v_*` views as **datasets**. Recommended in Superset:

- Mark `datetime` as the **temporal column** for time-series charts.
- Use `name` / `type` / `bus` as dimensions; `value` as the metric.
- Always keep a `scenario` filter (one Monte-Carlo path) unless you are
  intentionally aggregating across scenarios with `probability_factor`
  (join `dim_scenario`).

> Open the file **read-only** if multiple Superset workers connect
> concurrently (`duckdb:///…?access_mode=read_only`).

## Performance: `--materialize`

By default `fact_*` are lazy VIEWs over the Parquet files (fast build, slower
queries — full UNPIVOT/union scan per query). Pass `--materialize` to build
them as stored TABLEs once:

```bash
duckdb/.venv/bin/python duckdb/build_gtopt_db.py --materialize \
    --results <run>/results --db duckdb/gtopt.duckdb
```

Measured on the 16-scene juan run (1.16 B source rows, long format): build
**~12 s**, file **~94 MB**, typical Superset queries **5–20 ms**.

## Launching Superset against it

```bash
bash duckdb/superset_quickstart.sh duckdb/gtopt.duckdb
```

Installs Superset in an isolated venv (`~/superset-venv`), creates an admin
user the first time, prints the SQLAlchemy URI, and starts the dev server on
http://localhost:8088. Re-runs are idempotent.

## Other limitations

- **Synthetic datetime.** `datetime` is `epoch + hours_from_start`; the month
  label is authoritative, the absolute date is illustrative.
- **Scope.** Built for one `results/` directory (one solve). Point `--results`
  at a different run to rebuild; do not glob across sibling run directories.
