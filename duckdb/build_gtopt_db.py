#!/usr/bin/env python3
"""Build a DuckDB semantic layer over a gtopt results directory.

gtopt writes its solution as Hive-partitioned Parquet datasets:

    <results>/<Collection>/<field>_<stream>.parquet/scene=N/phase=M/part.parquet

Each dataset is *wide* -- the index columns ``scenario, stage, block`` followed
by one ``uid:<N>`` column per element (generator, bus, ...).  That shape is
ideal for the solver but unusable by BI tools (Superset, Metabase, Grafana),
which need *long/tidy* rows::

    scene | phase | scenario | stage | block | uid | name | ... | value

This script builds a DuckDB database that exposes exactly that, without copying
or rewriting any Parquet file.  It creates, as lazy SQL views over the original
files:

* ``dim_*``   -- dimension tables decoded from ``planning.json`` (generators,
                buses, lines, reservoirs, the stage/block calendar, ...).
* ``fact_*``  -- one long-format view per stream (wide->long via ``UNPIVOT``),
                with the ``uid:<N>`` columns folded into a clean integer ``uid``.
* ``v_*``     -- enriched views joining each fact to its dimension (names, type,
                bus, ...) and to the calendar (month, hours, synthetic datetime).
* ``meta_streams`` -- a catalog of every stream that was discovered.

The original Parquet files are never modified; everything here is a view.

Usage::

    python build_gtopt_db.py --results /path/to/results --db gtopt.duckdb
    duckdb gtopt.duckdb -c "SELECT * FROM v_generator_generation_sol LIMIT 5"
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import duckdb

# Index columns that are never element values (the rest are ``uid:<N>``).
INDEX_COLS = ("scene", "phase", "scenario", "stage", "block")

# Collections whose ``uid:<N>`` columns reference a system ``*_array`` element.
# (Derived automatically from the collection name; listed here only as docs.)
_DERIVED = "<Collection> -> system.<collection_snake_case>_array"


def to_snake(name: str) -> str:
    """``ReservoirDischargeLimit`` -> ``reservoir_discharge_limit``."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def q(ident: str) -> str:
    """Quote a SQL identifier, escaping embedded double quotes."""
    return '"' + ident.replace('"', '""') + '"'


def scalar_fields(rows: list[dict]) -> list[str]:
    """Field names that are scalar (int/float/str/bool) across every row.

    Nested list/dict fields (e.g. ``segments``) are dropped so a dimension
    table stays one row per element.
    """
    nested: set[str] = set()
    order: list[str] = []
    for row in rows:
        for key, val in row.items():
            if key not in order:
                order.append(key)
            if isinstance(val, (list, dict)):
                nested.add(key)
    return [k for k in order if k not in nested]


def build_dimensions(
    con: duckdb.DuckDBPyConnection, planning: dict, planning_path: str
) -> dict[str, list[str]]:
    """Create ``dim_<name>`` tables for every non-empty system ``*_array``.

    Returns a map ``collection_snake -> [scalar field names]`` so the caller
    can build enriched views.
    """
    read = (
        f"read_json('{planning_path}', maximum_object_size => 400000000)"
    )
    dims: dict[str, list[str]] = {}
    for key, rows in planning.get("system", {}).items():
        if not key.endswith("_array") or not isinstance(rows, list) or not rows:
            continue
        base = key[: -len("_array")]  # generator_array -> generator
        fields = scalar_fields(rows)
        if "uid" not in fields:
            continue
        cols = ", ".join(f"e.{q(f)} AS {q(f)}" for f in fields)
        con.execute(
            f"CREATE OR REPLACE TABLE dim_{base} AS "
            f"SELECT {cols} FROM (SELECT unnest(system.{key}) AS e FROM {read})"
        )
        dims[base] = fields
    return dims


def build_calendar(
    con: duckdb.DuckDBPyConnection,
    planning_path: str,
    planning: dict,
    epoch: str,
) -> None:
    """Create the stage/block/scenario/phase/scene dims and ``dim_time``.

    ``dim_time`` maps each ``(stage, block)`` to its month (if the run records
    one), block duration, cumulative ``hours_from_start`` and a synthetic
    ``datetime`` anchored at ``epoch``.  The wanted column list is intersected
    with what's actually present in ``planning.json`` so the layer works across
    gtopt schema variants (e.g. older runs carry ``discount_factor``/``month``,
    LP-only runs carry ``chronological`` and may drop ``month``).
    """
    read = f"read_json('{planning_path}', maximum_object_size => 400000000)"

    def unnest_table(table: str, array: str, wanted: list[str]) -> list[str]:
        rows = planning.get("simulation", {}).get(array, [])
        if not rows:
            return []
        available = scalar_fields(rows)
        cols = [c for c in wanted if c in available]
        if "uid" not in cols:
            return []
        sel = ", ".join(f"e.{q(c)} AS {q(c)}" for c in cols)
        con.execute(
            f"CREATE OR REPLACE TABLE {table} AS "
            f"SELECT {sel} FROM "
            f"(SELECT unnest(simulation.{array}) AS e FROM {read})"
        )
        return cols

    stage_cols = unnest_table(
        "dim_stage",
        "stage_array",
        ["uid", "active", "first_block", "count_block",
         "discount_factor", "month", "chronological"],
    )
    unnest_table("dim_block", "block_array", ["uid", "duration"])
    unnest_table("dim_scenario", "scenario_array",
                 ["uid", "probability_factor", "hydrology"])
    unnest_table("dim_scene", "scene_array",
                 ["uid", "first_scenario", "count_scenario"])
    unnest_table("dim_phase", "phase_array",
                 ["uid", "first_stage", "count_stage"])

    # Cumulative hours over the global block ordering.
    con.execute(
        "CREATE OR REPLACE TABLE dim_block_cum AS "
        "SELECT uid, duration, "
        "  sum(duration) OVER (ORDER BY uid) - duration AS hours_from_start, "
        "  sum(duration) OVER (ORDER BY uid) AS hours_to_end "
        "FROM dim_block"
    )
    # A stage owns the global blocks (first_block, first_block + count_block].
    month_sel = "s.month, " if "month" in stage_cols else ""
    con.execute(
        "CREATE OR REPLACE VIEW dim_time AS "
        f"SELECT s.uid AS stage, b.uid AS block, {month_sel}"
        "       b.duration, b.hours_from_start, "
        f"      (TIMESTAMP '{epoch}' + (b.hours_from_start * INTERVAL 1 HOUR)) "
        "         AS datetime "
        "FROM dim_stage s JOIN dim_block_cum b "
        "  ON b.uid > s.first_block AND b.uid <= s.first_block + s.count_block"
    )


def parquet_columns(con: duckdb.DuckDBPyConnection, glob: str) -> list[str]:
    """Column names of a Parquet dataset (partition-aware, schema-unioned)."""
    rows = con.execute(
        f"DESCRIBE SELECT * FROM read_parquet('{glob}', "
        "hive_partitioning=true, union_by_name=true) LIMIT 0"
    ).fetchall()
    return [r[0] for r in rows]


def build_streams(
    con: duckdb.DuckDBPyConnection,
    results: Path,
    dims: dict[str, list[str]],
    materialize: bool = False,
) -> list[tuple[str, str, str, str, bool]]:
    """Create ``fact_*`` and ``v_*`` views for every discovered stream.

    When ``materialize`` is set, each ``fact_*`` is built as a TABLE (the
    long-format rows are computed once and stored) instead of a lazy VIEW.
    This trades a one-off build cost and disk for fast Superset queries.

    Returns catalog rows ``(collection, field, kind, dim, has_calendar)``.
    """
    fact_kind = "TABLE" if materialize else "VIEW"
    # Is `month` available in the calendar (gtopt schema variant)?
    time_cols = {r[0] for r in con.execute("DESCRIBE dim_time").fetchall()}
    has_month = "month" in time_cols

    catalog: list[tuple[str, str, str, str, bool]] = []
    for ds_dir in sorted(results.glob("*/*.parquet")):
        if not ds_dir.is_dir():
            continue
        collection = ds_dir.parent.name
        stream = ds_dir.stem  # e.g. generation_sol
        coll_snake = to_snake(collection)
        glob = f"{ds_dir.as_posix()}/**/*.parquet"

        cols = parquet_columns(con, glob)
        is_wide = any(c.startswith("uid:") for c in cols)
        is_long = "uid" in cols and "value" in cols
        if not (is_wide or is_long):
            continue  # not a recognised gtopt stream

        view = f"{coll_snake}_{stream}"
        if is_wide:
            # Wide -> long: UNPIVOT, fold uid:<N> columns into integer uid.
            # COLUMNS('^uid:') selects only value columns; id columns survive
            # UNPIVOT as identifier columns; UNPIVOT drops NULLs by default.
            idx = [c for c in cols if not c.startswith("uid:")]
            con.execute(
                f"CREATE OR REPLACE {fact_kind} fact_{view} AS "
                "SELECT * EXCLUDE (col), "
                "       CAST(split_part(col, ':', 2) AS INTEGER) AS uid "
                "FROM (UNPIVOT (SELECT * FROM read_parquet("
                f"        '{glob}', hive_partitioning=true, union_by_name=true)) "
                "      ON COLUMNS('^uid:') INTO NAME col VALUE value)"
            )
        else:
            # Long-format gtopt output: already (..., uid, value) per row, but
            # the file still null-pads a full scene x phase grid -- drop NULLs
            # so the long table contains each model cell exactly once.
            idx = [c for c in cols if c not in ("uid", "value")]
            con.execute(
                f"CREATE OR REPLACE {fact_kind} fact_{view} AS "
                "SELECT * FROM read_parquet("
                f"   '{glob}', hive_partitioning=true, union_by_name=true) "
                "WHERE value IS NOT NULL"
            )

        # Enriched view: + dimension attributes + calendar.
        # scene/phase are dropped here: on the non-null cells they are a 1:1
        # relabel of scenario/stage (the wide files null-pad a full
        # scene x phase grid in every partition, filling only the diagonal).
        # Keeping them in v_ would invite a 320x SUM over the padded grid;
        # fact_ retains them for traceability.
        has_dim = coll_snake in dims
        has_cal = "stage" in idx and "block" in idx
        vidx = [c for c in idx if c not in ("scene", "phase")]
        sel = [f"f.{q(c)}" for c in vidx]
        joins = ""
        if has_cal:
            if has_month:
                sel.append("t.month")
            sel += [
                "t.duration",
                "t.hours_from_start",
                "t.datetime",
            ]
        sel.append("f.uid")
        if has_dim:
            for fld in dims[coll_snake]:
                if fld == "uid":
                    continue
                sel.append(f"d.{q(fld)} AS {q(fld)}")
            joins += f" LEFT JOIN dim_{coll_snake} d ON d.uid = f.uid"
        if has_cal:
            joins += " LEFT JOIN dim_time t ON t.stage = f.stage AND t.block = f.block"
        sel.append("f.value")
        con.execute(
            f"CREATE OR REPLACE VIEW v_{view} AS "
            f"SELECT {', '.join(sel)} FROM fact_{view} f{joins}"
        )

        field, _, kind = stream.rpartition("_")
        catalog.append(
            (collection, field or stream, kind or "", coll_snake if has_dim else "", has_cal)
        )
    return catalog


def main() -> int:
    here = Path(__file__).resolve().parent
    default_results = here.parent / "support" / "juan" / "results"

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--results",
        type=Path,
        default=default_results,
        help="gtopt results directory (contains planning.json) "
        f"[default: {default_results}]",
    )
    ap.add_argument(
        "--db",
        type=Path,
        default=here / "gtopt.duckdb",
        help="output DuckDB file [default: %(default)s]",
    )
    ap.add_argument(
        "--epoch",
        default="2020-01-01 00:00:00",
        help="synthetic datetime anchor for hour 0 [default: %(default)s]",
    )
    ap.add_argument(
        "--materialize",
        action="store_true",
        help="build fact_* as stored TABLEs (slower build, fast queries) "
        "instead of lazy VIEWs",
    )
    args = ap.parse_args()

    results = args.results.resolve()
    planning_path = results / "planning.json"
    if not planning_path.exists():
        ap.error(f"planning.json not found under {results}")

    planning = json.loads(planning_path.read_text())

    args.db.unlink(missing_ok=True)
    con = duckdb.connect(str(args.db))

    print(f"results : {results}")
    print(f"db      : {args.db}")
    dims = build_dimensions(con, planning, planning_path.as_posix())
    print(f"dims    : {len(dims)} dimension tables  ({', '.join(sorted(dims))})")

    build_calendar(con, planning_path.as_posix(), planning, args.epoch)
    print(f"calendar: dim_time anchored at {args.epoch}")

    catalog = build_streams(con, results, dims, materialize=args.materialize)
    if args.materialize:
        print("fact_*  : materialized as stored tables")
    con.execute(
        "CREATE OR REPLACE TABLE meta_streams "
        "(collection VARCHAR, field VARCHAR, kind VARCHAR, "
        " dimension VARCHAR, has_calendar BOOLEAN)"
    )
    if catalog:
        con.executemany(
            "INSERT INTO meta_streams VALUES (?, ?, ?, ?, ?)", catalog
        )
    print(f"streams : {len(catalog)} fact_/v_ view pairs")
    con.close()

    print("\nTry:")
    print(f"  duckdb {args.db} -c "
          '"FROM meta_streams"')
    print(f"  duckdb {args.db} -c "
          '"SELECT name, type, value FROM v_generator_generation_sol '
          "WHERE scenario=51 AND stage=1 AND block=1 AND value>0 "
          'ORDER BY value DESC LIMIT 5\"')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
