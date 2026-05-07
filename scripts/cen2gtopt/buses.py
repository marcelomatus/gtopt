#!/usr/bin/env python
# SPDX-License-Identifier: BSD-3-Clause
"""``cen_buses`` — Chilean SEN bus catalogue + operational-island viewer.

Subcommands
-----------

``list``        Print the local bus catalogue.
``show``        Show metadata for one bus (by id, bar_transf, or name).
``categorize``  Group catalogue rows by region × voltage.
``refresh``     Re-discover the catalogue (from local cache or live API).
``islands``     Inspect operational islands in a parquet dataset
                produced by ``cen_marginal_period.py``.

Examples
--------

::

    # Build / refresh the catalogue from already-cached cmg_real
    python -m cen2gtopt.buses refresh --from-cache

    # Force a fresh API discovery (slow ~10-30 min)
    python -m cen2gtopt.buses refresh --from-api --date 2026-04-22

    # List all buses (default)
    python -m cen2gtopt.buses list

    # Filter while listing
    python -m cen2gtopt.buses list --region centre --min-voltage 220

    # Show one bus
    python -m cen2gtopt.buses show CHARRUA
    python -m cen2gtopt.buses show 2061

    # Group by (region, voltage) — quick overview
    python -m cen2gtopt.buses categorize

    # Operational islands from a previously written dataset
    python -m cen2gtopt.buses islands --dataset /tmp/cen_marginal/ \\
        --date 2026-04-22 --quarter 24

    # Aggregate: which buses tend to share an island in a period?
    python -m cen2gtopt.buses islands --dataset /tmp/cen_marginal/ \\
        --aggregate
"""

from __future__ import annotations

import argparse
import logging
import os
import re
import sys
from pathlib import Path

import pandas as pd
import pyarrow.dataset as pads

from cen2gtopt._bus_catalogue import (
    DEFAULT_CATALOGUE_PATH,
    discover_buses_from_api,
    discover_buses_from_cache,
    filter_catalogue,
    load_catalogue,
    merge_catalogues,
    save_catalogue,
)
from cen2gtopt._cen_client import CenApiClient, CenApiConfig


def _make_client() -> CenApiClient:
    keys = {"sip": os.environ.get("CEN_USER_KEY", "492a5028ad67fd63791e28591cd29c01")}
    cfg = CenApiConfig(user_keys=keys, verify_tls=False)
    return CenApiClient(cfg)


# ---------------------------------------------------------------------------
# `list` / `show` / `categorize`
# ---------------------------------------------------------------------------


def _print_catalogue(df: pd.DataFrame) -> None:
    if df.empty:
        print("(catalogue empty — run `cen_buses refresh` first)")
        return
    pd.set_option("display.width", 220)
    pd.set_option("display.max_colwidth", 60)
    print(df.to_string(index=False))
    print(f"\n{len(df)} buses")


def _resolve_bus_ref(catalogue: pd.DataFrame, ref: str) -> pd.DataFrame:
    """Look up a bus by id_info, bar_transf, or substring of barra_info."""
    if catalogue.empty:
        return catalogue
    # Numeric → bus_id
    if ref.isdigit():
        return catalogue[catalogue["bus_id"] == int(ref)]
    # Try bar_transf exact match
    exact = catalogue[catalogue["bar_transf"].str.lower() == ref.lower()]
    if not exact.empty:
        return exact
    # Substring match on barra_info or bar_transf
    return catalogue[
        catalogue["barra_info"]
        .astype(str)
        .str.contains(re.escape(ref), case=False, na=False)
        | catalogue["bar_transf"]
        .astype(str)
        .str.contains(re.escape(ref), case=False, na=False)
    ]


def _cmd_list(args: argparse.Namespace) -> int:
    cat = load_catalogue(args.catalogue)
    cat = filter_catalogue(
        cat,
        name_regex=args.name_regex,
        region=args.region,
        min_voltage=args.min_voltage,
    )
    _print_catalogue(cat)
    return 0


def _cmd_show(args: argparse.Namespace) -> int:
    cat = load_catalogue(args.catalogue)
    if cat.empty:
        print("(catalogue empty — run `cen_buses refresh` first)")
        return 1
    matches = _resolve_bus_ref(cat, args.ref)
    if matches.empty:
        print(f"no bus matched {args.ref!r}")
        return 1
    if len(matches) > 1:
        print(f"{len(matches)} matches for {args.ref!r}:")
    pd.set_option("display.width", 220)
    pd.set_option("display.max_colwidth", 80)
    print(matches.to_string(index=False))
    return 0


def _cmd_categorize(args: argparse.Namespace) -> int:
    cat = load_catalogue(args.catalogue)
    if cat.empty:
        print("(catalogue empty — run `cen_buses refresh` first)")
        return 1
    grouped = (
        cat.groupby(["region", "voltage_kv"])
        .size()
        .rename("n_buses")
        .reset_index()
        .sort_values(["region", "voltage_kv"])
    )
    pivot = (
        grouped.pivot(index="region", columns="voltage_kv", values="n_buses")
        .fillna(0)
        .astype(int)
    )
    pivot["TOTAL"] = pivot.sum(axis=1)
    pivot.loc["TOTAL"] = pivot.sum(axis=0)
    print("Buses by (region × voltage_kV):")
    print(pivot.to_string())
    print(f"\n{len(cat)} buses total in catalogue")
    return 0


# ---------------------------------------------------------------------------
# `refresh`
# ---------------------------------------------------------------------------


def _cmd_refresh(args: argparse.Namespace) -> int:
    if args.from_api and not args.date:
        print("--from-api requires --date YYYY-MM-DD")
        return 2
    existing = load_catalogue(args.catalogue)
    n_before = len(existing)
    new_rows = pd.DataFrame()

    if args.from_api:
        print(f"discovering buses from API for {args.date} (this is slow…)")
        with _make_client() as client:
            new_rows = discover_buses_from_api(client, date=args.date)
        print(f"  → {len(new_rows)} buses fetched from API")
    else:
        print("discovering buses from local cache …")
        new_rows = discover_buses_from_cache()
        print(f"  → {len(new_rows)} buses found in cache")

    merged = merge_catalogues(existing, new_rows)
    path = save_catalogue(merged, args.catalogue)
    n_after = len(merged)
    n_added = n_after - n_before
    print(f"  catalogue: {n_before} → {n_after} buses (+{n_added})")
    print(f"  written:   {path}")
    return 0


# ---------------------------------------------------------------------------
# `islands` — operational-island viewer over a parquet dataset
# ---------------------------------------------------------------------------


def _load_dataset(path: Path) -> pd.DataFrame:
    """Read a Hive-partitioned parquet dataset produced by
    ``cen_marginal_period.py``."""
    if not path.exists():
        raise SystemExit(f"dataset not found: {path}")
    ds = pads.dataset(str(path), partitioning="hive")
    return ds.to_table().to_pandas()


def _cmd_islands_snapshot(df: pd.DataFrame, *, date_iso: str, quarter: int) -> int:
    """Show island composition for one (date, quarter) cell."""
    sub = df[(df["date_utc"] == date_iso) & (df["quarter"] == quarter)]
    if sub.empty:
        print(f"no rows for date={date_iso} quarter={quarter}")
        return 1
    pd.set_option("display.width", 220)
    pd.set_option("display.max_colwidth", 50)
    print(
        f"=== Operational islands at {date_iso} quarter={quarter} "
        f"({sub['hour'].iloc[0]:02d}:{sub['minute'].iloc[0]:02d}) ==="
    )
    grouped = (
        sub.groupby("island_id")
        .agg(
            n_buses=("bus_id", "count")
            if "bus_id" in sub.columns
            else ("bus_uid", "count"),
            mean_lambda=("lambda_cen", "mean"),
            min_lambda=("lambda_cen", "min"),
            max_lambda=("lambda_cen", "max"),
            marginal=(
                "marginal_central",
                lambda s: s.mode().iloc[0] if not s.mode().empty else "",
            ),
            fuel=(
                "marginal_fuel",
                lambda s: s.mode().iloc[0] if not s.mode().empty else "",
            ),
        )
        .round(2)
        .sort_values("mean_lambda")
    )
    print(grouped.to_string())
    print()
    print("--- buses per island ---")
    label_col = "bus_label" if "bus_label" in sub.columns else "barra_info"
    for island_id, grp in sub.groupby("island_id"):
        names = sorted(set(grp[label_col].astype(str)))
        print(
            f"  {island_id} ({len(names)} buses): {', '.join(names[:6])}"
            + (f" … (+{len(names) - 6} more)" if len(names) > 6 else "")
        )
    return 0


def _cmd_islands_aggregate(df: pd.DataFrame) -> int:
    """Aggregate analysis — typical island groupings across the period."""
    pd.set_option("display.width", 220)
    pd.set_option("display.max_colwidth", 60)
    print(f"=== Aggregate island statistics over {len(df)} cells ===")
    print(f"period: {df['date_utc'].min()} → {df['date_utc'].max()}")
    print(f"distinct islands across period: {df['island_id'].nunique()}")
    print()

    # Distribution of "number of islands" per (date, quarter) snapshot
    n_isl_per_q = df.groupby(["date_utc", "quarter"])["island_id"].nunique()
    print("Number-of-islands distribution per quarter:")
    print(n_isl_per_q.value_counts().sort_index().to_string())
    print()
    print(
        f"  median: {int(n_isl_per_q.median())}, "
        f"max: {int(n_isl_per_q.max())}, "
        f"single-island quarters: "
        f"{(n_isl_per_q == 1).sum()} / {len(n_isl_per_q)}"
    )
    print()

    # Island-size distribution (how many buses per island, on average)
    bus_col = "bus_id" if "bus_id" in df.columns else "bus_uid"
    sizes = df.groupby(["date_utc", "quarter", "island_id"])[bus_col].nunique()
    print("Island-size distribution (buses per island):")
    print(sizes.describe().round(2).to_string())
    print()

    # Co-island frequency: for each pair of buses, fraction of quarters
    # they share an island.  Capped at top-30 buses by frequency to
    # keep output readable.
    label_col = "bus_label" if "bus_label" in df.columns else "barra_info"
    top_buses = df.groupby(bus_col)[label_col].first().head(30)
    if len(top_buses) >= 2:
        # Build a bus × island crosstab, then bus × bus co-occurrence.
        labels = top_buses.to_dict()
        sub = df[df[bus_col].isin(top_buses.index)].copy()
        sub["bus_label"] = sub[bus_col].map(labels)
        # Sample to the first 200 quarters to keep runtime sane on
        # very large datasets (purely informational anyway).
        sample_keys = sub[["date_utc", "quarter"]].drop_duplicates().head(200)
        sub = sub.merge(sample_keys, on=["date_utc", "quarter"])
        cross = pd.crosstab(
            [sub["date_utc"], sub["quarter"], sub["island_id"]],
            sub["bus_label"],
        )
        cross = (cross > 0).astype(int)
        # Co-island matrix = X.T @ X / (number of quarters)
        n_q = cross.index.droplevel("island_id").drop_duplicates().size
        if n_q > 0:
            co = cross.T.dot(cross) / n_q
            # zero out diagonal
            for c in co.columns:
                co.loc[c, c] = 0.0
            print(
                "Co-island fraction (top-30 buses × top-30 buses, "
                f"sampled across {n_q} quarters):"
            )
            # Show top-10 most-tightly-bound bus pairs
            stack = co.stack().sort_values(ascending=False)
            stack = stack[stack < 1.0]  # exclude self
            seen: set[frozenset] = set()
            print()
            print(f"{'bus A':40s}  {'bus B':40s}  freq")
            for (a, b), v in stack.items():
                key = frozenset({a, b})
                if key in seen:
                    continue
                seen.add(key)
                if len(seen) > 10:
                    break
                print(f"{a:40s}  {b:40s}  {v:.2%}")
    return 0


def _cmd_islands(args: argparse.Namespace) -> int:
    df = _load_dataset(args.dataset)
    # Best-effort rename: legacy datasets used `bus_uid`, new ones use `bus_id`
    if "bus_id" not in df.columns and "bus_uid" in df.columns:
        df = df.rename(columns={"bus_uid": "bus_id"})

    if args.aggregate:
        return _cmd_islands_aggregate(df)
    if args.date and args.quarter is not None:
        return _cmd_islands_snapshot(df, date_iso=args.date, quarter=int(args.quarter))
    if args.date:
        # Default to peak quarter (18:00 = quarter 72)
        return _cmd_islands_snapshot(df, date_iso=args.date, quarter=72)

    # No args: print a one-line summary per available date
    print("dates available in dataset:")
    for d in sorted(df["date_utc"].unique()):
        n_isl = df[df["date_utc"] == d]["island_id"].nunique()
        print(f"  {d}: {n_isl} distinct islands across the day")
    print(
        "\nUse --date YYYY-MM-DD [--quarter Q] for a snapshot view, "
        "or --aggregate for period statistics."
    )
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--catalogue",
        type=Path,
        default=DEFAULT_CATALOGUE_PATH,
        help=f"Catalogue path (default: {DEFAULT_CATALOGUE_PATH})",
    )

    sub = p.add_subparsers(dest="cmd")
    sub.required = True

    pl = sub.add_parser("list", help="Print the catalogue")
    pl.add_argument("--name-regex", help="Regex on barra_info (case-insens.)")
    pl.add_argument(
        "--region",
        choices=("north", "north-centre", "centre", "south-centre", "south", "unknown"),
    )
    pl.add_argument("--min-voltage", type=int)
    pl.set_defaults(func=_cmd_list)

    ps = sub.add_parser("show", help="Show one bus")
    ps.add_argument("ref", help="bus_id, bar_transf, or substring of name")
    ps.set_defaults(func=_cmd_show)

    pc = sub.add_parser("categorize", help="Group by region × voltage")
    pc.set_defaults(func=_cmd_categorize)

    pr = sub.add_parser("refresh", help="Rebuild the catalogue")
    src = pr.add_mutually_exclusive_group(required=True)
    src.add_argument(
        "--from-cache",
        action="store_true",
        help="Discover from already-cached cmg_real parquets",
    )
    src.add_argument(
        "--from-api",
        action="store_true",
        help="Discover via paginated cmg_real API call (slow)",
    )
    pr.add_argument("--date", help="YYYY-MM-DD (required for --from-api)")
    pr.set_defaults(func=_cmd_refresh)

    pi = sub.add_parser(
        "islands", help="View operational islands in a marginal-period parquet dataset"
    )
    pi.add_argument(
        "--dataset", type=Path, required=True, help="Path to the parquet dataset root"
    )
    pi.add_argument("--date", help="YYYY-MM-DD")
    pi.add_argument(
        "--quarter", type=int, help="Quarter index 0..95 (default: 72 = 18:00)"
    )
    pi.add_argument(
        "--aggregate",
        action="store_true",
        help="Period-wide statistics instead of a snapshot",
    )
    pi.set_defaults(func=_cmd_islands)

    p.add_argument(
        "-v", "--verbose", action="store_true", help="Verbose logging (DEBUG level)."
    )
    p.add_argument(
        "-q", "--quiet", action="store_true", help="Suppress all output below WARNING."
    )
    p.add_argument("-V", "--version", action="version", version="cen2gtopt.buses 0.2.0")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if getattr(args, "quiet", False):
        log_level = logging.WARNING
    elif getattr(args, "verbose", False):
        log_level = logging.DEBUG
    else:
        log_level = logging.INFO
    logging.basicConfig(
        level=log_level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
