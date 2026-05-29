#!/usr/bin/env python3
"""Build a Superset dashboard against a gtopt DuckDB file via the REST API.

Idempotent: re-running updates rather than duplicating.  Adds a database
connection, the datasets needed for Generator / Bus / Line / Demand, a set of
charts, and a dashboard that hosts them.

Usage::

    bash duckdb/superset_quickstart.sh duckdb/mip_fuelfix_k4.duckdb &   # start server
    duckdb/.venv/bin/python -m pip install --quiet requests              # one-off
    SUPERSET_PASSWORD=... duckdb/.venv/bin/python duckdb/build_dashboard.py \\
        --db duckdb/mip_fuelfix_k4.duckdb --name "gtopt k4 overview"

If ``SUPERSET_PASSWORD`` is unset the script prompts.
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import sys
from pathlib import Path

import requests


# --- Superset HTTP wrapper ------------------------------------------------

class Superset:
    def __init__(self, url: str, user: str, password: str) -> None:
        self.url = url.rstrip("/")
        self.s = requests.Session()
        r = self.s.post(
            f"{self.url}/api/v1/security/login",
            json={
                "username": user,
                "password": password,
                "provider": "db",
                "refresh": True,
            },
            timeout=30,
        )
        r.raise_for_status()
        self.s.headers["Authorization"] = f"Bearer {r.json()['access_token']}"
        # CSRF token is required for POSTs that change state.
        r = self.s.get(f"{self.url}/api/v1/security/csrf_token/", timeout=30)
        r.raise_for_status()
        self.s.headers["X-CSRFToken"] = r.json()["result"]
        self.s.headers["Referer"] = self.url

    # ---- low-level helpers
    def get(self, path: str, **kw):
        r = self.s.get(f"{self.url}{path}", timeout=60, **kw)
        r.raise_for_status()
        return r.json()

    def post(self, path: str, payload: dict):
        r = self.s.post(f"{self.url}{path}", json=payload, timeout=60)
        if not r.ok:
            raise RuntimeError(f"POST {path} -> {r.status_code}: {r.text[:400]}")
        return r.json()

    def put(self, path: str, payload: dict):
        r = self.s.put(f"{self.url}{path}", json=payload, timeout=60)
        if not r.ok:
            raise RuntimeError(f"PUT {path} -> {r.status_code}: {r.text[:400]}")
        return r.json()

    # ---- domain helpers
    def find_database(self, name: str) -> int | None:
        q = {"filters": [{"col": "database_name", "opr": "eq", "value": name}]}
        res = self.get(f"/api/v1/database/?q={requests.utils.quote(json.dumps(q))}")
        items = res.get("result", [])
        return items[0]["id"] if items else None

    def upsert_database(self, name: str, uri: str) -> int:
        db_id = self.find_database(name)
        payload = {
            "database_name": name,
            "sqlalchemy_uri": uri,
            "expose_in_sqllab": True,
            "allow_ctas": False,
            "allow_cvas": False,
            "allow_dml": False,
        }
        if db_id is None:
            db_id = self.post("/api/v1/database/", payload)["id"]
            print(f"  + database {name} (id={db_id})")
        else:
            self.put(f"/api/v1/database/{db_id}", payload)
            print(f"  ~ database {name} (id={db_id})")
        return db_id

    def find_dataset(self, db_id: int, table: str, schema: str = "main") -> int | None:
        q = {
            "filters": [
                {"col": "database", "opr": "rel_o_m", "value": db_id},
                {"col": "table_name", "opr": "eq", "value": table},
            ]
        }
        res = self.get(f"/api/v1/dataset/?q={requests.utils.quote(json.dumps(q))}")
        items = res.get("result", [])
        return items[0]["id"] if items else None

    def upsert_dataset(self, db_id: int, table: str, schema: str = "main") -> int:
        ds_id = self.find_dataset(db_id, table, schema)
        if ds_id is None:
            ds_id = self.post(
                "/api/v1/dataset/",
                {"database": db_id, "schema": schema, "table_name": table},
            )["id"]
            print(f"  + dataset {table} (id={ds_id})")
        else:
            print(f"  ~ dataset {table} (id={ds_id})")
        return ds_id

    def find_chart(self, name: str) -> int | None:
        q = {"filters": [{"col": "slice_name", "opr": "eq", "value": name}]}
        res = self.get(f"/api/v1/chart/?q={requests.utils.quote(json.dumps(q))}")
        items = res.get("result", [])
        return items[0]["id"] if items else None

    def upsert_chart(self, ds_id: int, name: str, viz_type: str, params: dict) -> int:
        body = {
            "slice_name": name,
            "viz_type": viz_type,
            "datasource_id": ds_id,
            "datasource_type": "table",
            "params": json.dumps({**params, "datasource": f"{ds_id}__table",
                                  "viz_type": viz_type}),
        }
        cid = self.find_chart(name)
        if cid is None:
            cid = self.post("/api/v1/chart/", body)["id"]
            print(f"  + chart  '{name}' (id={cid}, {viz_type})")
        else:
            self.put(f"/api/v1/chart/{cid}", body)
            print(f"  ~ chart  '{name}' (id={cid}, {viz_type})")
        return cid

    def find_dashboard(self, title: str) -> int | None:
        q = {"filters": [{"col": "dashboard_title", "opr": "eq", "value": title}]}
        res = self.get(f"/api/v1/dashboard/?q={requests.utils.quote(json.dumps(q))}")
        items = res.get("result", [])
        return items[0]["id"] if items else None

    def upsert_dashboard(self, title: str, position_json: dict) -> int:
        body = {
            "dashboard_title": title,
            "position_json": json.dumps(position_json),
            "published": True,
        }
        did = self.find_dashboard(title)
        if did is None:
            did = self.post("/api/v1/dashboard/", body)["id"]
            print(f"  + dashboard '{title}' (id={did})")
        else:
            self.put(f"/api/v1/dashboard/{did}", body)
            print(f"  ~ dashboard '{title}' (id={did})")
        return did


# --- chart factories ------------------------------------------------------

def _metric_sum_value(label: str = "MW (sum)") -> dict:
    return {
        "label": label,
        "expressionType": "SIMPLE",
        "column": {"column_name": "value", "type": "DOUBLE"},
        "aggregate": "SUM",
        "optionName": "metric_sum_value",
    }


def _metric_energy(label: str = "Energy (GWh)") -> dict:
    return {
        "label": label,
        "expressionType": "SQL",
        "sqlExpression": "SUM(value * duration) / 1000.0",
        "optionName": "metric_energy",
    }


def _metric_avg_value(label: str) -> dict:
    return {
        "label": label,
        "expressionType": "SIMPLE",
        "column": {"column_name": "value", "type": "DOUBLE"},
        "aggregate": "AVG",
        "optionName": "metric_avg_value",
    }


def _filter_value_positive() -> dict:
    return {
        "clause": "WHERE",
        "expressionType": "SIMPLE",
        "subject": "value",
        "operator": ">",
        "comparator": "0",
    }


def chart_gen_by_fuel(ds: int) -> tuple[str, str, dict]:
    return (
        "Generation by fuel type",
        "echarts_timeseries_bar",
        {
            "x_axis": "datetime",
            "metrics": [_metric_sum_value()],
            "groupby": ["type"],
            "stack": "Stack",
            "adhoc_filters": [_filter_value_positive()],
            "row_limit": 50000,
            "x_axis_title": "Time",
            "y_axis_title": "MW",
            "show_legend": True,
        },
    )


def chart_total_gen(ds: int) -> tuple[str, str, dict]:
    return (
        "Total system generation",
        "echarts_timeseries_line",
        {
            "x_axis": "datetime",
            "metrics": [_metric_sum_value("Total MW")],
            "adhoc_filters": [_filter_value_positive()],
            "row_limit": 10000,
            "x_axis_title": "Time",
            "y_axis_title": "MW",
        },
    )


def chart_top_generators(ds: int) -> tuple[str, str, dict]:
    return (
        "Top generators by energy",
        "table",
        {
            "groupby": ["name", "type"],
            "metrics": [_metric_energy()],
            "row_limit": 20,
            "order_desc": True,
            "include_search": True,
        },
    )


def chart_bus_lmp_avg(ds: int) -> tuple[str, str, dict]:
    return (
        "Average bus LMP (top 15)",
        "echarts_timeseries_bar",
        {
            "x_axis": "name",
            "metrics": [_metric_avg_value("LMP ($/MWh)")],
            "groupby": [],
            "row_limit": 15,
            "order_desc": True,
            "orientation": "horizontal",
        },
    )


def chart_bus_lmp_over_time(ds: int) -> tuple[str, str, dict]:
    return (
        "Bus LMP over time (top 10 buses)",
        "echarts_timeseries_line",
        {
            "x_axis": "datetime",
            "metrics": [_metric_avg_value("LMP ($/MWh)")],
            "groupby": ["name"],
            "row_limit": 10000,
            "series_limit": 10,
            "series_limit_metric": _metric_avg_value("LMP ($/MWh)"),
        },
    )


def chart_top_lines(ds: int) -> tuple[str, str, dict]:
    return (
        "Top lines by avg flow (positive)",
        "echarts_timeseries_bar",
        {
            "x_axis": "name",
            "metrics": [_metric_avg_value("Avg flow (MW)")],
            "row_limit": 15,
            "order_desc": True,
            "orientation": "horizontal",
        },
    )


def chart_line_flows_over_time(ds: int) -> tuple[str, str, dict]:
    return (
        "Top 10 line flows over time",
        "echarts_timeseries_line",
        {
            "x_axis": "datetime",
            "metrics": [_metric_avg_value("Avg flow (MW)")],
            "groupby": ["name"],
            "row_limit": 10000,
            "series_limit": 10,
            "series_limit_metric": _metric_avg_value("Avg flow (MW)"),
        },
    )


# --- dashboard layout -----------------------------------------------------

def make_position(chart_ids_by_name: dict[str, int]) -> dict:
    """Build a position_json that arranges the charts in a 12-col grid."""
    rows: list[tuple[str, ...]] = [
        ("Generation by fuel type", "Top generators by energy"),
        ("Total system generation",),
        ("Bus LMP over time (top 10 buses)", "Average bus LMP (top 15)"),
        ("Top 10 line flows over time", "Top lines by avg flow (positive)"),
    ]

    pos = {
        "DASHBOARD_VERSION_KEY": "v2",
        "ROOT_ID": {"type": "ROOT", "id": "ROOT_ID", "children": ["GRID_ID"]},
        "GRID_ID": {"type": "GRID", "id": "GRID_ID", "children": [],
                    "parents": ["ROOT_ID"]},
    }

    for ri, row_names in enumerate(rows, start=1):
        row_id = f"ROW-{ri}"
        pos[row_id] = {
            "type": "ROW", "id": row_id, "children": [],
            "parents": ["ROOT_ID", "GRID_ID"],
            "meta": {"background": "BACKGROUND_TRANSPARENT"},
        }
        pos["GRID_ID"]["children"].append(row_id)
        width_per = 12 // len(row_names)
        for ci, name in enumerate(row_names):
            chart_id = chart_ids_by_name.get(name)
            if chart_id is None:
                continue
            comp_id = f"CHART-{ri}-{ci}"
            pos[comp_id] = {
                "type": "CHART", "id": comp_id,
                "parents": ["ROOT_ID", "GRID_ID", row_id],
                "meta": {
                    "chartId": chart_id,
                    "width": width_per,
                    "height": 50,
                    "sliceName": name,
                },
                "children": [],
            }
            pos[row_id]["children"].append(comp_id)
    return pos


# --- main -----------------------------------------------------------------

DATASETS_NEEDED = [
    "v_generator_generation_sol",
    "v_bus_balance_dual",
    "v_line_flowp_sol",
    "v_demand_load_sol",
]


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--db", type=Path,
                    default=here / "mip_fuelfix_k4.duckdb",
                    help="DuckDB file to expose [default: %(default)s]")
    ap.add_argument("--name", default="gtopt overview",
                    help="dashboard title (and DB connection name)")
    ap.add_argument("--url", default="http://localhost:8088",
                    help="Superset base URL")
    ap.add_argument("--user", default="admin")
    args = ap.parse_args()

    db_path = args.db.resolve()
    if not db_path.exists():
        ap.error(f"DuckDB file not found: {db_path}")

    pw = os.environ.get("SUPERSET_PASSWORD") or getpass.getpass(
        f"Superset password for {args.user}: "
    )

    print(f"connecting to {args.url} as {args.user}")
    try:
        s = Superset(args.url, args.user, pw)
    except requests.RequestException as e:
        print(f"FAIL: cannot reach Superset at {args.url}: {e}")
        return 2

    print("provisioning database + datasets")
    uri = f"duckdb:///{db_path}?access_mode=read_only"
    db_id = s.upsert_database(args.name, uri)

    ds_ids: dict[str, int] = {}
    for name in DATASETS_NEEDED:
        ds_ids[name] = s.upsert_dataset(db_id, name)

    print("creating charts")
    gen_ds = ds_ids["v_generator_generation_sol"]
    bus_ds = ds_ids["v_bus_balance_dual"]
    lin_ds = ds_ids["v_line_flowp_sol"]

    chart_factories = [
        (gen_ds, chart_gen_by_fuel),
        (gen_ds, chart_total_gen),
        (gen_ds, chart_top_generators),
        (bus_ds, chart_bus_lmp_avg),
        (bus_ds, chart_bus_lmp_over_time),
        (lin_ds, chart_top_lines),
        (lin_ds, chart_line_flows_over_time),
    ]
    chart_ids: dict[str, int] = {}
    for ds_id, factory in chart_factories:
        name, viz, params = factory(ds_id)
        try:
            chart_ids[name] = s.upsert_chart(ds_id, name, viz, params)
        except RuntimeError as e:
            print(f"  ! chart '{name}' failed: {e}")

    print("assembling dashboard")
    position = make_position(chart_ids)
    dash_id = s.upsert_dashboard(args.name, position)

    print(f"\nDONE.  Open: {args.url}/superset/dashboard/{dash_id}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
