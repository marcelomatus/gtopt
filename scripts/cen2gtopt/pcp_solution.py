# SPDX-License-Identifier: BSD-3-Clause
"""``cen2gtopt.pcp_solution`` — read CEN PLEXOS .accdb solution files.

Inspects and extracts data from the
``Model PRGdia_Full_Definitivo*Solution.accdb`` Microsoft Access
databases shipped inside every ``RES{date}.zip`` (PCP) and inside
``Modelos/Model PRGdia_Full_Definitivo_PID Solution/*.accdb`` (PID).

Why this is the ground-truth oracle
-----------------------------------

The .accdb is the **PLEXOS solver output** — it contains every dual
variable and every primal value that CEN's day-ahead / intra-day LP
produced.  Specifically:

  * **Per-(generator, period) Generation MW**     — the real dispatch
    used in settlement.  Resolves the ambiguity of "which unit was
    interior at hour H".
  * **Per-(node, period) Price (USD/MWh)**       — the LMP at every
    bus.  Direct comparison to ``cmg-real`` and to our pipeline's
    ``lambda_pipe``.
  * **Per-(generator, period) ShadowPrice**      — Lagrange dual on
    the unit's commitment / pmax constraint, useful for
    near-marginal-unit identification.
  * **Per-(line, period) Flow + Loss**           — line-level energy
    flows and loss contributions; resolves congestion / loss
    decomposition (PJM-style LMP split).

System requirement
------------------

This module shells out to the ``mdb-tables`` and ``mdb-export``
binaries from the **mdbtools** package — *not* a Python wheel.
Install once via the OS package manager:

::

    sudo apt-get install -y mdbtools          # Debian / Ubuntu
    brew install mdbtools                      # macOS
    conda install -c conda-forge mdbtools      # conda env

(See also the ``[project.optional-dependencies].mdb`` extra in
``scripts/pyproject.toml`` — ``pip install ./scripts[mdb]`` adds the
optional ``pandas-access`` Python wrapper.)

If ``mdbtools`` is not on ``$PATH``, every CLI subcommand exits 4
with a clear install-instruction message.

Examples
--------

::

    # List every table in the solution DB
    python -m cen2gtopt.pcp_solution tables --date 2026-04-07

    # Dump one table to CSV
    python -m cen2gtopt.pcp_solution dump \\
        --date 2026-04-07 --table Period_0 \\
        --output /tmp/period0.csv

    # Headline triangulation: PLEXOS LMPs vs cmg-real
    python -m cen2gtopt.pcp_solution lmp --date 2026-04-07
"""

from __future__ import annotations

import argparse
import io
import logging
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

import pandas as pd

from cen2gtopt.pcp_archive import (
    DEFAULT_DOWNLOAD_ROOT,
    download_one,
    fetch_index,
)

_LOG = logging.getLogger("cen2gtopt.pcp_solution")

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_NO_DATA = 3
EXIT_NO_MDBTOOLS = 4


# ---------------------------------------------------------------------------
# mdbtools probe + thin wrappers
# ---------------------------------------------------------------------------


def _have_mdbtools() -> bool:
    """True iff both ``mdb-tables`` and ``mdb-export`` are on $PATH."""
    return all(shutil.which(b) for b in ("mdb-tables", "mdb-export"))


def _require_mdbtools() -> None:
    if not _have_mdbtools():
        raise SystemExit(
            "mdbtools not found on $PATH.\n"
            "Install via your OS package manager:\n"
            "  Debian/Ubuntu:  sudo apt-get install -y mdbtools\n"
            "  macOS:          brew install mdbtools\n"
            "  conda env:      conda install -c conda-forge mdbtools\n"
            "(see scripts/pyproject.toml [project.optional-dependencies].mdb)"
        )


def list_tables(accdb_path: Path) -> list[str]:
    """Return every table name in the .accdb (via ``mdb-tables -1``)."""
    _require_mdbtools()
    out = subprocess.run(
        ["mdb-tables", "-1", str(accdb_path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    return [line.strip() for line in out.stdout.splitlines() if line.strip()]


def export_table(
    accdb_path: Path,
    table: str,
    *,
    timeout: int = 600,
) -> pd.DataFrame:
    """Read one table to a DataFrame via ``mdb-export``."""
    _require_mdbtools()
    out = subprocess.run(
        ["mdb-export", "-D", "%Y-%m-%d %H:%M:%S", str(accdb_path), table],
        check=True,
        capture_output=True,
        text=False,  # bytes — let pandas detect encoding
        timeout=timeout,
    )
    if not out.stdout:
        return pd.DataFrame()
    return pd.read_csv(io.BytesIO(out.stdout), low_memory=False)


@lru_cache(maxsize=64)
def _cached_export_table(accdb_path_str: str, table: str) -> pd.DataFrame:
    """Process-lifetime cache around :func:`export_table`.

    Each `mdb-export` call costs ~0.5–1 s and ``extract_property`` reads
    six tables.  Calling it twice on the same accdb (e.g. ``Price`` then
    ``Marginal Loss Factor``) used to re-read all six tables.  This
    cache turns the second call into a near-zero-cost lookup of four of
    those six tables (``t_property``, ``t_key``, ``t_membership``,
    ``t_object``), dropping a 5 s second call to ~0.5 s.

    The cache is keyed by (path-string, table-name).  Callers must
    treat the returned DataFrame as read-only — filter / rename
    operations that pandas implements as new frames are safe; in-place
    mutations would corrupt the cache.
    """
    return export_table(Path(accdb_path_str), table)


def clear_table_cache() -> None:
    """Drop the in-memory cache of mdbtools table reads.

    Call between back-to-back batches over different accdb files
    if the cache footprint becomes large.
    """
    _cached_export_table.cache_clear()


# ---------------------------------------------------------------------------
# Resolve the .accdb path for a given date
# ---------------------------------------------------------------------------


@dataclass
class SolutionPaths:
    accdb_path: Path
    source: str  # "PCP" or "PID"
    period: int | None  # PID period, None for PCP


def _ymd(date_iso: str) -> str:
    return date_iso.replace("-", "")


def resolve_solution(
    date_iso: str,
    *,
    period: int | None = None,
    source: str = "auto",
    download_root: Path | None = None,
    extract_root: Path | None = None,
    auto_download: bool = True,
) -> SolutionPaths:
    """Locate the PLEXOS solution .accdb for ``date_iso``.

    Order of preference when ``source="auto"``:
      1. PID for the requested period (or latest if not specified)
      2. PCP for that date
    """
    download_root = download_root or DEFAULT_DOWNLOAD_ROOT
    extract_root = extract_root or (download_root / "_unpacked")
    ymd = _ymd(date_iso)

    if source in ("auto", "pid"):
        # First check the local extract cache — works without network.
        local_pid_dirs = (
            sorted(
                extract_root.glob(f"PID_{ymd}_*"),
                reverse=True,
            )
            if extract_root.exists()
            else []
        )
        if period is not None:
            local_pid_dirs = [
                d for d in local_pid_dirs if d.name == f"PID_{ymd}_{period:02d}"
            ]
        for pid_dir in local_pid_dirs:
            inner = pid_dir / pid_dir.name
            sol_dir = inner / "Modelos" / "Model PRGdia_Full_Definitivo_PID Solution"
            cand = list(sol_dir.glob("*.accdb"))
            if cand:
                eff_period = period
                if eff_period is None:
                    m = re.search(r"PID_\d{8}_(\d{2})$", pid_dir.name)
                    if m:
                        eff_period = int(m.group(1))
                return SolutionPaths(
                    accdb_path=cand[0],
                    source="PID",
                    period=eff_period,
                )

        # Otherwise consult the remote index (auto_download path)
        files = fetch_index() if auto_download else []
        pid_candidates = sorted(
            [f for f in files if f.nombre.startswith(f"PID_{ymd}_")],
            key=lambda f: f.nombre,
            reverse=True,
        )
        if period is not None:
            wanted = f"PID_{ymd}_{period:02d}.zip"
            pid_candidates = [f for f in pid_candidates if f.nombre == wanted]
        if pid_candidates:
            pid_zip = download_root / "PID" / pid_candidates[0].nombre
            if not pid_zip.exists() and auto_download:
                _LOG.info("downloading %s …", pid_candidates[0].nombre)
                download_one(pid_candidates[0], output_dir=download_root)
            extract_dir = extract_root / pid_candidates[0].nombre.removesuffix(".zip")
            if not (
                extract_dir / pid_candidates[0].nombre.removesuffix(".zip") / "Modelos"
            ).exists():
                extract_dir.mkdir(parents=True, exist_ok=True)
                with zipfile.ZipFile(pid_zip) as z:
                    z.extractall(extract_dir)
            inner = extract_dir / pid_candidates[0].nombre.removesuffix(".zip")
            sol_dir = inner / "Modelos" / "Model PRGdia_Full_Definitivo_PID Solution"
            cand = list(sol_dir.glob("*.accdb"))
            if cand:
                eff_period = period
                if eff_period is None:
                    m = re.search(
                        r"PID_\d{8}_(\d{2})$",
                        pid_candidates[0].nombre.removesuffix(".zip"),
                    )
                    if m:
                        eff_period = int(m.group(1))
                return SolutionPaths(
                    accdb_path=cand[0],
                    source="PID",
                    period=eff_period,
                )

    if source == "pid":
        raise FileNotFoundError(
            f"no PID .accdb for {date_iso}" + (f" period {period}" if period else "")
        )

    # PCP fall-back
    plexos_zip = download_root / "PCP" / f"PLEXOS{ymd}.zip"
    if not plexos_zip.exists() and auto_download:
        files = fetch_index()
        match = [f for f in files if f.nombre == f"PLEXOS{ymd}.zip"]
        if not match:
            raise FileNotFoundError(f"no PLEXOS{ymd}.zip for {date_iso}")
        download_one(match[0], output_dir=download_root)

    extract_dir = extract_root / f"PCP_RES_{ymd}"
    sol_dir = extract_dir / ("Model PRGdia_Full_Definitivo Solution")
    if not sol_dir.exists():
        # Unpack outer ZIP, then unpack RES.zip
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            with zipfile.ZipFile(plexos_zip) as z:
                z.extractall(tmp)
            res_zip = tmp / f"RES{ymd}.zip"
            if not res_zip.exists():
                raise FileNotFoundError(f"no RES inside {plexos_zip}")
            extract_dir.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(res_zip) as z:
                z.extractall(extract_dir)

    cand = list(sol_dir.glob("*.accdb"))
    if not cand:
        raise FileNotFoundError(f"no .accdb in {sol_dir}")
    return SolutionPaths(accdb_path=cand[0], source="PCP", period=None)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _cmd_tables(args: argparse.Namespace) -> int:
    sol = resolve_solution(args.date, period=args.period, source=args.source)
    label = f"{sol.source}" + (f" period={sol.period:02d}" if sol.period else "")
    print(f"=== {label} solution → {sol.accdb_path} ===")
    print(f"  size: {sol.accdb_path.stat().st_size / 1e6:.1f} MB")
    tables = list_tables(sol.accdb_path)
    print(f"  {len(tables)} tables:")
    for t in tables:
        print(f"    {t}")
    return EXIT_OK


def _cmd_dump(args: argparse.Namespace) -> int:
    sol = resolve_solution(args.date, period=args.period, source=args.source)
    df = export_table(sol.accdb_path, args.table)
    if df.empty:
        print(f"table {args.table!r} is empty or absent")
        return EXIT_NO_DATA
    print(f"table {args.table!r}: {len(df)} rows × {len(df.columns)} cols")
    if args.output:
        df.to_csv(args.output, index=False)
        print(f"  wrote {args.output}")
    else:
        pd.set_option("display.width", 220)
        pd.set_option("display.max_columns", 30)
        print(df.head(args.head).to_string(index=False))
    return EXIT_OK


def extract_property(
    accdb_path: Path,
    *,
    property_name: str,
    collection_id: int,
    period_table: int = 0,
) -> pd.DataFrame:
    """Extract one PLEXOS property's full time series via the schema joins.

    The PLEXOS solution DB is normalised:

      ``t_data_{N} (key_id, period_id, value)``
        ↓ key_id
      ``t_key (key_id, membership_id, property_id, period_type_id, ...)``
        ↓ membership_id
      ``t_membership (membership_id, parent_object_id, child_object_id,
                      collection_id)``
        ↓ child_object_id
      ``t_object (object_id, name, class_id)``

      ``t_data_{N}.period_id`` ↔ ``t_period_{N}.interval_id``
        gives the wall-clock ``datetime``.

    ``t_property`` defines named properties scoped to one collection_id.
    For Node-class Price (CEN PCP/PID), pass
    ``property_name='Price', collection_id=281`` (System.Nodes).

    Args:
        accdb_path: path to the .accdb solution file.
        property_name: e.g. ``'Price'``, ``'Generation'``, ``'Losses'``.
        collection_id: e.g. ``281`` for System.Nodes, ``1`` for
            System.Generators (auto-discoverable via t_collection).
        period_table: which ``t_data_{N}`` / ``t_period_{N}`` pair to
            read (0 = interval-level — the default for the PCP/PID
            hourly time-series).

    Returns a DataFrame with columns:
      * ``object_name`` — the bus / generator name
      * ``datetime``    — period timestamp
      * ``value``       — the property value (e.g. USD/MWh for Price)
      * ``hour``        — derived from ``datetime``
      * ``period_id``   — raw PLEXOS period id
    """
    _require_mdbtools()
    # All six table reads route through ``_cached_export_table`` so that
    # back-to-back ``extract_property`` calls on the same accdb (e.g.
    # Price then Marginal Loss Factor) reuse the joined schema tables.
    accdb_key = str(accdb_path)
    # 1. Find property_id for (name, collection_id)
    prop = _cached_export_table(accdb_key, "t_property")
    match = prop[
        (prop["name"] == property_name) & (prop["collection_id"] == collection_id)
    ]
    if match.empty:
        raise ValueError(
            f"no property name={property_name!r} in collection_id={collection_id}; "
            f"check t_property"
        )
    property_id = int(match["property_id"].iloc[0])
    _LOG.info(
        "property_id=%d (%s, collection %d)",
        property_id,
        property_name,
        collection_id,
    )

    # 2. t_key rows for that property (filter to interval-level data)
    t_key = _cached_export_table(accdb_key, "t_key")
    keys = t_key[
        (t_key["property_id"] == property_id)
        & (t_key["period_type_id"] == period_table)
    ][["key_id", "membership_id"]]
    if keys.empty:
        raise ValueError(
            f"no t_key rows for property {property_id} at period_type {period_table}"
        )
    _LOG.info("t_key matches: %d", len(keys))

    # 3. Join t_membership → t_object to get the bus name
    t_mem = _cached_export_table(accdb_key, "t_membership")
    t_mem = t_mem[t_mem["collection_id"] == collection_id][
        ["membership_id", "child_object_id"]
    ]
    t_obj = _cached_export_table(accdb_key, "t_object")[["object_id", "name"]]

    keys_with_obj = keys.merge(t_mem, on="membership_id", how="inner")
    keys_with_obj = keys_with_obj.merge(
        t_obj.rename(columns={"object_id": "child_object_id", "name": "object_name"}),
        on="child_object_id",
        how="inner",
    )
    _LOG.info("keys joined to %d objects", keys_with_obj["child_object_id"].nunique())

    # 4. Read t_data_{period_table} and join on key_id
    data = _cached_export_table(accdb_key, f"t_data_{period_table}")
    data = data[data["key_id"].isin(keys_with_obj["key_id"])]
    out = data.merge(
        keys_with_obj[["key_id", "object_name"]],
        on="key_id",
        how="inner",
    )

    # 5. Join t_period_{period_table} for wall-clock datetime
    period = _cached_export_table(accdb_key, f"t_period_{period_table}")
    period = period.rename(columns={"interval_id": "period_id"})[
        ["period_id", "datetime"]
    ]
    out = out.merge(period, on="period_id", how="left")
    out["datetime"] = pd.to_datetime(out["datetime"], errors="coerce")
    out["hour"] = out["datetime"].dt.hour
    return out[["object_name", "datetime", "hour", "period_id", "value"]]


def _cmd_lmp(args: argparse.Namespace) -> int:
    """Extract per-(node, hour) LMPs from the PLEXOS solution DB."""
    sol = resolve_solution(args.date, period=args.period, source=args.source)
    print(
        f"=== {sol.source}"
        + (f" period={sol.period:02d}" if sol.period else "")
        + f" → {sol.accdb_path.name} ==="
    )
    df = extract_property(
        sol.accdb_path,
        property_name="Price",
        collection_id=281,  # System.Nodes
        period_table=0,
    )
    if df.empty:
        print("no Price data extracted")
        return EXIT_NO_DATA
    df = df.rename(columns={"object_name": "bus_name", "value": "lmp_usd_mwh"})
    print(
        f"rows: {len(df):,}; buses: {df['bus_name'].nunique()}; "
        f"hours: {df['hour'].nunique()}"
    )
    print()
    pd.set_option("display.width", 220)
    pd.set_option("display.float_format", lambda x: f"{x:.2f}")

    # Hourly system summary
    by_hour = (
        df.groupby("hour")["lmp_usd_mwh"]
        .agg(["mean", "median", "min", "max", "count"])
        .round(2)
    )
    print("--- hourly system LMP (across all nodes) ---")
    print(by_hour.head(24).to_string())
    print()
    # Top 5 buses by mean LMP
    top = (
        df.groupby("bus_name")["lmp_usd_mwh"]
        .mean()
        .sort_values(ascending=False)
        .head(8)
        .round(2)
    )
    print("--- top 8 buses by mean LMP ---")
    print(top.to_string())
    print()
    bottom = df.groupby("bus_name")["lmp_usd_mwh"].mean().sort_values().head(5).round(2)
    print("--- bottom 5 buses by mean LMP ---")
    print(bottom.to_string())

    if args.output:
        df.to_parquet(args.output, index=False)
        print(f"\n  wrote {args.output}")
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cen2gtopt.pcp_solution",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("-q", "--quiet", action="store_true")
    p.add_argument(
        "-V", "--version", action="version", version="cen2gtopt.pcp_solution 0.1.0"
    )

    sub = p.add_subparsers(dest="cmd")
    sub.required = True

    def _common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--date", required=True, help="YYYY-MM-DD")
        sp.add_argument("--source", default="auto", choices=("auto", "pcp", "pid"))
        sp.add_argument(
            "--period", type=int, default=None, help="PID re-solve period (1..24)"
        )

    pt = sub.add_parser("tables", help="List tables in the solution DB")
    _common(pt)
    pt.set_defaults(func=_cmd_tables)

    pd_ = sub.add_parser("dump", help="Dump one table")
    _common(pd_)
    pd_.add_argument("--table", required=True)
    pd_.add_argument("--output", type=Path, help="Write CSV instead of head-printing")
    pd_.add_argument("--head", type=int, default=20)
    pd_.set_defaults(func=_cmd_dump)

    pl = sub.add_parser("lmp", help="Extract per-node LMPs from solution")
    _common(pl)
    pl.add_argument("--output", type=Path, help="Write parquet")
    pl.set_defaults(func=_cmd_lmp)

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
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
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
