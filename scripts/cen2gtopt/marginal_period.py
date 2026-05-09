#!/usr/bin/env python
# SPDX-License-Identifier: BSD-3-Clause
"""Multi-day orchestrator for the CEN marginal-unit identifier (v3).

Runs ``compute_marginal_units_for_day`` (from
``tools/cen_emission_factor_v3.py``) over a date range — a week, a
month, a year, or longer — in parallel, and writes a Hive-partitioned
parquet dataset suitable for direct DuckDB / pandas / pyarrow
consumption.

Period definition
-----------------

Pick one of:

  --start-date YYYY-MM-DD --end-date YYYY-MM-DD     (inclusive)
  --year YYYY                                       (Jan 1 → Dec 31)
  --month YYYY-MM                                   (whole month)
  --last-n-days N                                   (today − N → today − 1)

Bus selection
-------------

Default: the 10 representative reference buses spanning N→S Chile
(``REF_BUSES`` in v3). Override with ``--buses csv`` (comma-separated
``bus_uid`` integers) to subset.

Parallelism
-----------

``--workers N`` uses ``multiprocessing.Pool`` (process-based, since
each day issues many CEN HTTP fetches that release the GIL).
Recommended: 4-8 workers (CEN's SIP API tolerates moderate
concurrency; one client per worker).

Output layout
-------------

A Hive-partitioned parquet dataset::

    --output /tmp/cen_marginal/
      ├── manifest.json                    # period metadata + per-day status
      └── year=YYYY/
          └── month=MM/
              └── day=DD/
                  └── data.parquet         # ~960 rows for 10 buses

Read it back with:

    import pyarrow.dataset as ds
    df = ds.dataset(out_path, partitioning='hive').to_table().to_pandas()

Resume
------

By default the orchestrator skips dates that already have a non-empty
``data.parquet``.  Pass ``--force`` to re-run.

Per-day failures (no published λ_CEN, network errors, etc.) are logged
to the manifest as ``status: error`` and do **not** abort the period.

Schema
------

Each row covers one (bus, 15-min quarter) cell and carries (per the
user's request + Tier-1 enrichment):

  * **base λ**: ``lambda_cen``, ``lambda_provenance``
  * **marginal unit**: ``marginal_central``, ``marginal_id_central``,
    ``marginal_fuel``, ``marginal_cv``, ``marginal_pmin/pmax``,
    ``marginal_dispatch_mw``, ``marginal_headroom_mw``, ``epsilon``
  * **constructed λ + correction**: ``loss_factor``,
    ``lambda_constructed`` (= ``marginal_cv × loss_factor``)
  * **constructed marginal emission**: ``epsilon_at_bus``,
    ``co2_tax_usd_per_mwh``
  * **island**: ``island_id``, ``island_n_buses``
  * **system context**: ``system_total_demand_mw``,
    ``system_renewable_fraction``
  * **diagnostics**: ``regime``, ``n_candidates``, ``n_forced_in_pool``,
    ``tier_wedge_usd``, ``top_below_lambda``, ``top_above_lambda``,
    ``delta_lmp``, ``abs_delta``, ``discrepancy_flag``

Example
-------

::

    # one full month, 6 workers
    python -m cen2gtopt.marginal_period \\
        --month 2026-04 --output /tmp/cen_marginal/ --workers 6

    # one full year on the default 10 buses (≈365 × 960 = 350k rows)
    python -m cen2gtopt.marginal_period \\
        --year 2026 --output /tmp/cen_marginal_2026/ --workers 8

    # back-fill last 7 days only
    python -m cen2gtopt.marginal_period \\
        --last-n-days 7 --output /tmp/cen_marginal/ --workers 4
"""

from __future__ import annotations

import argparse
import json
import logging
import multiprocessing as mp
import os
import sys
import time
import traceback
from dataclasses import dataclass, asdict, field
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from cen2gtopt._bus_catalogue import (
    discover_buses_from_api,
    discover_buses_from_cache,
    filter_catalogue,
    load_catalogue,
    merge_catalogues,
    save_catalogue,
    to_ref_buses,
)
from cen2gtopt._cen_client import CenApiClient, CenApiConfig
from cen2gtopt._emission_factors import (
    UpstreamBasis,
    add_upstream_basis_arg,
    parse_upstream_basis,
)
from cen2gtopt.marginal_units import (
    REF_BUSES,
    SIP_KEY,
    compute_marginal_units_for_day,
)

_LOG = logging.getLogger("cen_marginal_period")


# ---------------------------------------------------------------------------
# Period parsing
# ---------------------------------------------------------------------------


def _today_clt() -> date:
    """Operating-day reference in CLT (UTC-3, no DST handling)."""
    return (datetime.now(timezone.utc) + timedelta(hours=-3)).date()


def parse_period(args: argparse.Namespace) -> tuple[date, date]:
    """Resolve the (start, end) inclusive period from CLI flags."""
    if args.start_date and args.end_date:
        return (
            date.fromisoformat(args.start_date),
            date.fromisoformat(args.end_date),
        )
    if args.year:
        y = int(args.year)
        return date(y, 1, 1), date(y, 12, 31)
    if args.month:
        y, m = (int(s) for s in args.month.split("-"))
        first = date(y, m, 1)
        nxt = date(y + 1, 1, 1) if m == 12 else date(y, m + 1, 1)
        return first, nxt - timedelta(days=1)
    if args.last_n_days:
        end = _today_clt() - timedelta(days=1)
        start = end - timedelta(days=int(args.last_n_days) - 1)
        return start, end
    raise SystemExit(
        "Must specify a period: --start-date+--end-date, --year, "
        "--month, or --last-n-days"
    )


def date_range(start: date, end: date) -> list[date]:
    out: list[date] = []
    cur = start
    while cur <= end:
        out.append(cur)
        cur += timedelta(days=1)
    return out


# ---------------------------------------------------------------------------
# Output layout (Hive-partitioned)
# ---------------------------------------------------------------------------


def day_parquet_path(out_root: Path, d: date) -> Path:
    """Hive-partitioned path: ``year=YYYY/month=MM/day=DD/data.parquet``."""
    return (
        out_root
        / f"year={d.year:04d}"
        / f"month={d.month:02d}"
        / f"day={d.day:02d}"
        / "data.parquet"
    )


def already_done(out_root: Path, d: date) -> bool:
    """A day is done when its parquet file exists and is non-empty."""
    p = day_parquet_path(out_root, d)
    return p.exists() and p.stat().st_size > 0


# ---------------------------------------------------------------------------
# Bus selection
# ---------------------------------------------------------------------------


def _parse_bus_ids(spec: str) -> set[int]:
    """Parse a comma-separated or @file.txt list of integer bus_ids."""
    out: set[int] = set()
    if spec.startswith("@"):
        path = Path(spec[1:])
        if not path.exists():
            raise SystemExit(f"--buses {spec}: file not found")
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                out.add(int(line.split()[0]))
            except ValueError:
                continue
        return out
    for x in spec.split(","):
        x = x.strip()
        if not x:
            continue
        try:
            out.add(int(x))
        except ValueError as exc:
            raise SystemExit(f"--buses: cannot parse {x!r} as integer bus_id") from exc
    return out


def select_ref_buses(
    *,
    buses_spec: str,
    name_regex: str | None,
    region: str | None,
    min_voltage: int | None,
    catalogue_path: Path | None,
) -> list[tuple[str, int, str]]:
    """Resolve CLI selectors to the (bar_transf, bus_id, label) tuples.

    Resolution order:

    1. ``buses_spec == "default10"`` → the v3 ``REF_BUSES`` (small, fast).
    2. ``buses_spec == "all"`` *(default)* → entire bus catalogue.
    3. ``buses_spec == "5,12,17"`` or ``"@file.txt"`` → explicit bus_ids
       intersected with the catalogue.

    Then filter the result through ``--name-regex``, ``--region``,
    ``--min-voltage`` (AND).

    Falls back to ``REF_BUSES`` when the catalogue is empty.
    """
    # Special case: small built-in subset, no catalogue lookup.
    if buses_spec == "default10":
        return list(REF_BUSES)

    catalogue = load_catalogue(catalogue_path)
    if catalogue.empty:
        # No catalogue yet — fall back gracefully.
        if buses_spec == "all":
            print(
                "  WARNING: catalogue empty — falling back to default10. "
                "Run `python tools/cen_buses.py refresh --from-cache` first."
            )
            return list(REF_BUSES)
        # An explicit list still works against REF_BUSES.
        wanted = _parse_bus_ids(buses_spec)
        return [b for b in REF_BUSES if b[1] in wanted] or list(REF_BUSES)

    bus_ids: set[int] | None = None
    if buses_spec != "all":
        bus_ids = _parse_bus_ids(buses_spec)

    filtered = filter_catalogue(
        catalogue,
        bus_ids=bus_ids,
        name_regex=name_regex,
        region=region,
        min_voltage=min_voltage,
    )
    return to_ref_buses(filtered)


def _maybe_refresh_catalogue(
    *,
    refresh: str | None,
    date: str | None,
    catalogue_path: Path | None,
) -> None:
    """Refresh the on-disk catalogue.  ``refresh`` is ``cache``,
    ``api`` or None."""
    if refresh is None:
        return
    existing = load_catalogue(catalogue_path)
    if refresh == "cache":
        new = discover_buses_from_cache()
    else:  # api
        if not date:
            raise SystemExit("--refresh-buses=api requires --refresh-date")
        sip_cfg = CenApiConfig(
            user_keys={"sip": SIP_KEY},
            verify_tls=False,
        )
        with CenApiClient(sip_cfg) as client:
            new = discover_buses_from_api(client, date=date)
    merged = merge_catalogues(existing, new)
    path = save_catalogue(merged, catalogue_path)
    print(f"  catalogue refreshed: {len(existing)} → {len(merged)} buses → {path}")


# ---------------------------------------------------------------------------
# Worker entry point (top-level → picklable for ProcessPool)
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class DayResult:
    date_iso: str
    status: str  # "ok" | "empty" | "skipped" | "error"
    n_rows: int = 0
    n_buses: int = 0
    error: str = ""
    duration_s: float = 0.0


def _process_one_day(
    job: tuple[str, list[tuple[str, int, str]], str, str, str],
) -> DayResult:
    """Worker function — one date.  Pickling-safe (top-level).

    Job tuple layout: ``(date_iso, ref_buses, cmg_source, out_root_str,
    upstream_basis_value)`` where ``upstream_basis_value`` is the
    string form of an :class:`UpstreamBasis` (passed as a string for
    ProcessPool pickle compatibility).
    """
    date_iso, ref_buses, cmg_source, out_root_str, basis_value = job
    upstream_basis = parse_upstream_basis(basis_value)
    out_root = Path(out_root_str)
    res = DayResult(date_iso=date_iso, status="?", n_buses=len(ref_buses))
    t0 = time.monotonic()
    try:
        sip_cfg = CenApiConfig(
            user_keys={"sip": SIP_KEY},
            verify_tls=False,
        )
        with CenApiClient(sip_cfg) as client:
            df = compute_marginal_units_for_day(
                client,
                date_iso=date_iso,
                ref_buses=ref_buses,
                cmg_source=cmg_source,
                verbose=False,
                upstream_basis=upstream_basis,
            )
        if df.empty:
            res.status = "empty"
        else:
            d = date.fromisoformat(date_iso)
            out_path = day_parquet_path(out_root, d)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            # Write with a stable schema (cast Int64 NA → int + null mask).
            tbl = pa.Table.from_pandas(df, preserve_index=False)
            pq.write_table(tbl, out_path, compression="zstd")
            res.status = "ok"
            res.n_rows = len(df)
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
        res.status = "error"
        res.error = (
            f"{type(exc).__name__}: {exc}\n"
            + "".join(traceback.format_exception(exc))[:2000]
        )
    res.duration_s = round(time.monotonic() - t0, 2)
    return res


# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class PeriodManifest:
    started_at: str
    finished_at: str = ""
    period_start: str = ""
    period_end: str = ""
    n_days_total: int = 0
    n_days_ok: int = 0
    n_days_empty: int = 0
    n_days_skipped: int = 0
    n_days_error: int = 0
    n_rows_total: int = 0
    workers: int = 0
    cmg_source: str = ""
    upstream_basis: str = UpstreamBasis.NONE.value
    ref_bus_uids: list[int] = field(default_factory=list)
    days: list[dict] = field(default_factory=list)

    def write(self, out_root: Path) -> Path:
        out_root.mkdir(parents=True, exist_ok=True)
        # Underscore prefix → pyarrow.dataset() ignores it (treats as
        # "hidden metadata"), so the manifest can live in the dataset
        # root without breaking partition discovery.
        path = out_root / "_manifest.json"
        path.write_text(json.dumps(asdict(self), indent=2, default=str))
        return path


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    g = p.add_argument_group("period (pick one)")
    g.add_argument("--start-date", help="YYYY-MM-DD (inclusive)")
    g.add_argument("--end-date", help="YYYY-MM-DD (inclusive)")
    g.add_argument("--year", type=int, help="Process whole calendar year")
    g.add_argument("--month", help="YYYY-MM — process whole month")
    g.add_argument(
        "--last-n-days",
        type=int,
        help="Process the last N completed days (today excluded)",
    )

    bg = p.add_argument_group("bus selection")
    bg.add_argument(
        "--buses",
        default="all",
        help="'all' (default — every bus in the catalogue), "
        "'default10' (the v3 REF_BUSES subset, fast), "
        "comma-separated bus_id list ('2061,2050,1234'), "
        "or '@file.txt' with one bus_id per line",
    )
    bg.add_argument(
        "--buses-name-regex",
        help="Filter buses by regex on barra_info "
        "(case-insensitive). AND with other selectors.",
    )
    bg.add_argument(
        "--buses-region",
        choices=("north", "north-centre", "centre", "south-centre", "south", "unknown"),
        help="Filter buses by inferred region.",
    )
    bg.add_argument(
        "--min-voltage",
        type=int,
        help="Drop buses with voltage_kv below this threshold "
        "(common values: 110, 154, 220, 500).",
    )
    bg.add_argument(
        "--catalogue",
        type=Path,
        default=None,
        help="Bus-catalogue path (default: "
        "~/.cache/gtopt/cen2gtopt/known_buses.parquet)",
    )
    bg.add_argument(
        "--list-buses",
        action="store_true",
        help="Print the resolved bus selection and exit.",
    )
    bg.add_argument(
        "--refresh-buses",
        choices=("cache", "api"),
        help="Refresh the catalogue before resolving selection. "
        "'cache' is fast; 'api' is slow (~10-30 min) and "
        "requires --refresh-date.",
    )
    bg.add_argument(
        "--refresh-date",
        help="Date for --refresh-buses=api (YYYY-MM-DD).",
    )
    bg.add_argument(
        "--i-know-this-is-big",
        action="store_true",
        help="Bypass safety prompt for runs estimated to exceed 100 000 cell-fetches.",
    )
    p.add_argument(
        "--cmg-source",
        default="best_real",
        choices=("real", "online", "best_real"),
        help="CMG source policy (default: best_real)",
    )
    p.add_argument(
        "--output", required=True, type=Path, help="Output dataset root directory"
    )
    p.add_argument(
        "--workers",
        type=int,
        default=min(6, max(1, os.cpu_count() or 4)),
        help="Process-pool size (default: min(6, ncpu))",
    )
    p.add_argument(
        "--force",
        action="store_true",
        help="Re-process days even if their parquet exists",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Print plan and exit (no fetch, no write)",
    )
    p.add_argument(
        "--max-failures",
        type=int,
        default=20,
        help="Abort if more than N days fail (default: 20)",
    )
    p.add_argument(
        "-v", "--verbose", action="store_true", help="Verbose logging (DEBUG level)."
    )
    p.add_argument(
        "-q", "--quiet", action="store_true", help="Suppress all output below WARNING."
    )
    p.add_argument(
        "-V", "--version", action="version", version="cen2gtopt.marginal_period 0.2.0"
    )
    add_upstream_basis_arg(p)
    return p


_BUDGET_THRESHOLD = 100_000


def main(argv: list[str] | None = None) -> int:
    p = build_parser()
    args = p.parse_args(argv)

    if args.quiet:
        log_level = logging.WARNING
    elif args.verbose:
        log_level = logging.DEBUG
    else:
        log_level = logging.INFO
    logging.basicConfig(
        level=log_level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    # Optional catalogue refresh BEFORE selection.
    _maybe_refresh_catalogue(
        refresh=args.refresh_buses,
        date=args.refresh_date,
        catalogue_path=args.catalogue,
    )

    start, end = parse_period(args)
    all_dates = date_range(start, end)
    ref_buses = select_ref_buses(
        buses_spec=args.buses,
        name_regex=args.buses_name_regex,
        region=args.buses_region,
        min_voltage=args.min_voltage,
        catalogue_path=args.catalogue,
    )

    if not ref_buses:
        raise SystemExit(
            "no buses matched the selectors; refine --buses / "
            "--buses-region / --buses-name-regex / --min-voltage"
        )

    if args.list_buses:
        print(f"Resolved bus selection — {len(ref_buses)} buses:")
        for bar_transf, bus_id, label in ref_buses:
            print(f"  {bus_id:>6}  {bar_transf:20s}  {label}")
        return 0

    # Resume: filter out already-done dates (unless --force)
    out_root: Path = args.output
    todo: list[date] = []
    skipped: list[date] = []
    for d in all_dates:
        if not args.force and already_done(out_root, d):
            skipped.append(d)
        else:
            todo.append(d)

    # Safety: refuse to launch huge runs without explicit consent.
    estimated_cells = len(todo) * len(ref_buses) * 96
    print("=" * 80)
    print(f"  cen_marginal_period — {start.isoformat()} → {end.isoformat()}")
    print("=" * 80)
    print(f"  total days:           {len(all_dates):>6}")
    print(f"  already done:         {len(skipped):>6} (--force to redo)")
    print(f"  to process:           {len(todo):>6}")
    print(f"  buses:                {len(ref_buses):>6}")
    print(f"  estimated cells:      {estimated_cells:>10,}")
    print(f"  cmg_source:           {args.cmg_source}")
    print(f"  output root:          {out_root}")
    print(f"  workers:              {args.workers}")

    if estimated_cells > _BUDGET_THRESHOLD and not args.i_know_this_is_big:
        print()
        print(
            f"  ⚠  estimated cells ({estimated_cells:,}) > "
            f"safety threshold ({_BUDGET_THRESHOLD:,})."
        )
        print(
            "     Add --i-know-this-is-big to proceed, or narrow the "
            "selection (--buses, --buses-region, --min-voltage, "
            "--start-date/--end-date)."
        )
        return 2

    if args.dry_run:
        print("\n  DRY RUN — exiting before any fetch.")
        return 0

    out_root.mkdir(parents=True, exist_ok=True)
    manifest = PeriodManifest(
        started_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        period_start=start.isoformat(),
        period_end=end.isoformat(),
        n_days_total=len(all_dates),
        n_days_skipped=len(skipped),
        workers=args.workers,
        cmg_source=args.cmg_source,
        upstream_basis=parse_upstream_basis(args.upstream_basis).value,
        ref_bus_uids=[b[1] for b in ref_buses],
    )

    upstream_basis = parse_upstream_basis(args.upstream_basis)
    jobs = [
        (
            d.isoformat(),
            ref_buses,
            args.cmg_source,
            str(out_root),
            upstream_basis.value,
        )
        for d in todo
    ]

    n_failures = 0
    n_done = 0
    n_total = len(jobs)
    if jobs:
        # Use 'spawn' to avoid forking issues with the requests session
        # held inside CenApiClient (already-open sockets in parent).
        ctx = mp.get_context("spawn")
        with ctx.Pool(processes=args.workers) as pool:
            for res in pool.imap_unordered(_process_one_day, jobs):
                n_done += 1
                manifest.days.append(asdict(res))
                if res.status == "ok":
                    manifest.n_days_ok += 1
                    manifest.n_rows_total += res.n_rows
                elif res.status == "empty":
                    manifest.n_days_empty += 1
                elif res.status == "error":
                    manifest.n_days_error += 1
                    n_failures += 1
                tag = {"ok": "✓", "empty": "∅", "error": "✗"}.get(res.status, "?")
                print(
                    f"  [{n_done:>4}/{n_total}] {tag} {res.date_iso}  "
                    f"rows={res.n_rows:>6}  "
                    f"dur={res.duration_s:>6.1f}s  {res.error[:60]}"
                )
                if n_failures > args.max_failures:
                    print(
                        f"\n  ABORT: {n_failures} failures > "
                        f"--max-failures={args.max_failures}"
                    )
                    pool.terminate()
                    pool.join()
                    break

    manifest.finished_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    manifest_path = manifest.write(out_root)
    print()
    print("=" * 80)
    print("  SUMMARY")
    print("=" * 80)
    print(f"  ok:         {manifest.n_days_ok:>4}")
    print(f"  empty:      {manifest.n_days_empty:>4}")
    print(f"  skipped:    {manifest.n_days_skipped:>4}")
    print(f"  error:      {manifest.n_days_error:>4}")
    print(f"  total rows: {manifest.n_rows_total:>9,}")
    print(f"  manifest:   {manifest_path}")
    print(f"  dataset:    {out_root}")
    print()
    print("  Read with:")
    print("    import pyarrow.dataset as ds")
    print(
        f"    df = ds.dataset('{out_root}', partitioning='hive').to_table().to_pandas()"
    )

    return 1 if manifest.n_days_error > args.max_failures else 0


if __name__ == "__main__":
    sys.exit(main())
