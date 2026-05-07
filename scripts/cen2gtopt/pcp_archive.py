# SPDX-License-Identifier: BSD-3-Clause
"""``cen2gtopt.pcp_archive`` — download CEN PLEXOS PCP base / solution files.

The Coordinador's "Programa de Operación" portal at
``https://programa.coordinador.cl/operacion/pcp/bases-modelo`` exposes
the *complete daily LP inputs and solution* of the Programa de
Coordinación de Predespacho (PCP).  These are the **ground-truth
oracle** for everything the marginal-emission pipeline reverse-
engineers from public CMG data.

Discovery mechanism
-------------------

The portal is an Angular SPA whose backend is an authenticated S3-
backed REST API at ``administracion.api.coordinador.cl/programa-
operacion/`` — but the SPA's bundled USERKEY constant is publicly
embedded (it is the "browse-only" key, not a per-user credential).
We use that key to call:

  GET …/programa-operacion/bucket-s3/s3/listFiles?user_key=<KEY>

…which returns ``[{ id, nombre, descripcion, enlaceDescarga,
fechaPublicacion, categoria, ... }]`` for every file currently
hosted, including pre-signed S3 URLs valid for ~12 h.

The presigned URLs let us download anonymously without per-request
auth.  When a URL expires the listing call is cheap (~0.5 s) and
returns a fresh URL.

File categories
---------------

The portal exposes ~1 900 files across these categories:

  PCP   — Programa de Coordinación de Predespacho (day-ahead full
          unit-commitment, hourly).  Includes per-day artefacts:
            * PLEXOS{YYYYMMDD}.zip      — full LP base + solution
              (DATOS{date}.zip + RES{date}.zip; 30-200 MB)
            * TCO{YYYYMMDD}.zip          — Política de Operación
            * OfertasSSCCAdj{date}.zip   — adjudicated SSCC offers
            * BasePCPDef{date}.xlsx      — input parameter sheets
  PID   — Programa Intra-Diario (hourly intra-day re-runs)
  PMP   — Programa de Coordinación de Mediano Plazo (weekly model)
  PLP   — Programa de Largo Plazo (long-term SDDP)
  IM    — Informes mensuales
  EC    — Estudios complementarios
  RE    — Reportes especiales

Use ``pcp_archive list`` to enumerate.

Bundle structure (PLEXOS{date}.zip)
-----------------------------------

::

    PLEXOS{YYYYMMDD}.zip
    ├── DATOS{YYYYMMDD}.zip          # LP inputs
    │   ├── DBSEN_PRGDIARIO.xml      # PLEXOS XML object database
    │   │                            # (4 533 objects, 96 classes)
    │   ├── Gen_HeatRate.csv         # per-unit heat rate
    │   ├── Gen_StartCost.csv
    │   ├── Gen_MinStableLevel.csv   # pmin per unit
    │   ├── Gen_Rating.csv           # pmax per unit
    │   ├── Gen_VOMCharge.csv        # variable O&M
    │   ├── Gen_FuelTransportCharge.csv
    │   ├── Fuel_Price.csv           # monthly fuel prices
    │   ├── Fuel_MaxOfftakeWeek.csv  # TOP / take-or-pay constraints
    │   ├── Hydro_StoWaterValues.csv # water values per reservoir
    │   ├── Hydro_WaterFlows.csv     # natural inflow time series
    │   ├── Hydro_MaxVolume.csv / MinVolume.csv
    │   ├── Hydro_InitialVolume.csv
    │   ├── Hydro_AntucoBounds.csv   # Antuco-specific bounds
    │   ├── Hydro_EfficiencyIncr.csv
    │   ├── Hydro_MaxRampDay.csv
    │   ├── Lin_MaxRating.csv / Lin_MinRating.csv
    │   ├── Lin_Units.csv            # line topology + units
    │   ├── Nod_Load.csv             # per-bus per-hour load (wide)
    │   ├── Res_Requirement.csv      # reserve requirements
    │   ├── Res_Timeslice.csv
    │   ├── ReserveUsageTxCompensation.csv
    │   ├── SSCC_Activation_BESS.csv # BESS reserve participation
    │   ├── BESS_IniValue.csv        # initial state-of-charge
    │   └── CFdata/CPF/, CSF/, CTF/  # per-unit reserve capacity
    │                                # factors (primario/secundario/
    │                                # terciario)
    └── RES{YYYYMMDD}.zip            # Solution
        └── Model PRGdia_Full_Definitivo Solution/
            ├── …Solution.accdb       # PLEXOS solution Access DB
            │                         # (LMPs, dispatch, duals — the
            │                         # ground-truth marginal labels)
            ├── …Solution.zip         # supplementary
            └── …Log.txt              # solver log

Examples
--------

::

    # List all available PLEXOS PCP files
    python -m cen2gtopt.pcp_archive list --pattern PLEXOS

    # Download one specific PCP day
    python -m cen2gtopt.pcp_archive download --name PLEXOS20260407.zip \\
        --output ~/cen_pcp/

    # Download every PLEXOS file from a date range
    python -m cen2gtopt.pcp_archive download --pattern PLEXOS \\
        --since 2026-04-01 --until 2026-04-30 --output ~/cen_pcp/

    # Show metadata for one file
    python -m cen2gtopt.pcp_archive info PLEXOS20260407.zip

    # Refresh the index (forces a fresh API call)
    python -m cen2gtopt.pcp_archive refresh
"""

from __future__ import annotations

import argparse
import json
import logging
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import requests

_LOG = logging.getLogger("cen2gtopt.pcp_archive")

#: Public USERKEY embedded in the Angular SPA at programa.coordinador.cl
#: (extracted from ``main-CP4OALKL.js`` rt.USERKEY).  This is the
#: browse-only key — NOT a per-user credential.  CEN may rotate it; if
#: requests start returning ``Authentication parameters missing``,
#: re-extract by grepping ``USERKEY`` in the SPA bundle.
DEFAULT_USERKEY = "f3cdad2758436a0a2c2c1fec92853de7"

LIST_URL = (
    "https://administracion.api.coordinador.cl/"
    "programa-operacion/bucket-s3/s3/listFiles"
)

#: On-disk index cache — the listFiles response is ~1 MB JSON.
DEFAULT_INDEX_PATH = Path.home() / ".cache" / "gtopt" / "cen2gtopt" / "pcp_index.json"

#: Default download root.
DEFAULT_DOWNLOAD_ROOT = Path.home() / ".cache" / "gtopt" / "cen2gtopt" / "pcp_archive"

#: How long the cached index is considered fresh.
INDEX_FRESH_S = 6 * 3600


@dataclass(slots=True)
class PcpFile:
    """One row of the listFiles response."""

    id: int
    nombre: str
    descripcion: str
    categoria: str
    path: str
    fecha_publicacion: str
    fecha_vigencia: str
    enlace_descarga: str
    vencimiento: str
    estado: str

    @classmethod
    def from_dict(cls, d: dict) -> "PcpFile":
        return cls(
            id=int(d.get("id", 0)),
            nombre=str(d.get("nombre", "")),
            descripcion=str(d.get("descripcion", "")),
            categoria=str(d.get("categoria", "")),
            path=str(d.get("path", "")),
            fecha_publicacion=str(d.get("fechaPublicacion", "")),
            fecha_vigencia=str(d.get("fechaVigencia", "")),
            enlace_descarga=str(d.get("enlaceDescarga", "")),
            vencimiento=str(d.get("vencimiento", "")),
            estado=str(d.get("estado", "")),
        )

    def date_iso(self) -> str | None:
        """Extract YYYY-MM-DD from a ``…YYYYMMDD…`` filename."""
        m = re.search(r"(\d{8})", self.nombre)
        if not m:
            return None
        s = m.group(1)
        return f"{s[:4]}-{s[4:6]}-{s[6:]}"


# ---------------------------------------------------------------------------
# Index fetch + cache
# ---------------------------------------------------------------------------


def fetch_index(
    *,
    user_key: str = DEFAULT_USERKEY,
    cache_path: Path | None = None,
    force: bool = False,
    timeout: int = 60,
) -> list[PcpFile]:
    """Return the list of available files.

    Uses an on-disk JSON cache (``DEFAULT_INDEX_PATH``).  Re-fetches
    when older than ``INDEX_FRESH_S`` (6 h) or when ``force=True``.
    Each call returns *fresh presigned URLs* in the live response —
    cached URLs may have expired (typically valid 12 h).
    """
    cache_path = cache_path or DEFAULT_INDEX_PATH
    if not force and cache_path.exists():
        age = time.time() - cache_path.stat().st_mtime
        if age < INDEX_FRESH_S:
            _LOG.debug("index cache hit (age=%.0fs)", age)
            data = json.loads(cache_path.read_text(encoding="utf-8"))
            return [PcpFile.from_dict(d) for d in data]

    _LOG.info("fetching CEN PCP index …")
    resp = requests.get(
        LIST_URL,
        params={"user_key": user_key},
        headers={"Origin": "https://programa.coordinador.cl"},
        timeout=timeout,
        verify=True,
    )
    resp.raise_for_status()
    rows = resp.json()
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_text(json.dumps(rows), encoding="utf-8")
    _LOG.info("  → %d files indexed → %s", len(rows), cache_path)
    return [PcpFile.from_dict(r) for r in rows]


# ---------------------------------------------------------------------------
# Filtering
# ---------------------------------------------------------------------------


def filter_files(
    files: list[PcpFile],
    *,
    name_pattern: str | None = None,
    category: str | None = None,
    since: str | None = None,
    until: str | None = None,
) -> list[PcpFile]:
    """AND-filter the file list."""
    out = files
    if name_pattern:
        rx = re.compile(name_pattern, re.IGNORECASE)
        out = [f for f in out if rx.search(f.nombre)]
    if category:
        out = [f for f in out if f.categoria.upper() == category.upper()]
    if since or until:
        new_out: list[PcpFile] = []
        for f in out:
            d = f.date_iso()
            if d is None:
                continue
            if since and d < since:
                continue
            if until and d > until:
                continue
            new_out.append(f)
        out = new_out
    return out


# ---------------------------------------------------------------------------
# Download
# ---------------------------------------------------------------------------


def download_one(
    f: PcpFile,
    *,
    output_dir: Path,
    skip_existing: bool = True,
    timeout: int = 600,
) -> Path:
    """Download ``f`` into ``output_dir / f.categoria / f.nombre``.
    Returns the local path."""
    out = output_dir / f.categoria / f.nombre
    out.parent.mkdir(parents=True, exist_ok=True)
    if skip_existing and out.exists() and out.stat().st_size > 0:
        _LOG.debug("skip existing: %s", out)
        return out
    _LOG.info("download %s (%s) …", f.nombre, f.descripcion[:60])
    with requests.get(
        f.enlace_descarga,
        stream=True,
        timeout=timeout,
        verify=True,
    ) as r:
        r.raise_for_status()
        tmp = out.with_suffix(out.suffix + ".part")
        with tmp.open("wb") as fp:
            for chunk in r.iter_content(chunk_size=1 << 20):
                if chunk:
                    fp.write(chunk)
        tmp.rename(out)
    _LOG.info("  → %s (%.1f MB)", out, out.stat().st_size / 1e6)
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _print_table(rows: list[PcpFile], limit: int | None = 30) -> None:
    print(f"{'name':40s}  {'category':6s}  {'date':10s}  description")
    print("-" * 110)
    for r in rows[:limit] if limit else rows:
        print(
            f"{r.nombre:40s}  {r.categoria:6s}  "
            f"{(r.date_iso() or '?'):10s}  {r.descripcion[:60]}"
        )
    if limit and len(rows) > limit:
        print(f"… and {len(rows) - limit} more (use --limit 0 to show all)")


def _cmd_list(args: argparse.Namespace) -> int:
    files = fetch_index(force=args.refresh)
    files = filter_files(
        files,
        name_pattern=args.pattern,
        category=args.category,
        since=args.since,
        until=args.until,
    )
    files.sort(key=lambda f: (f.date_iso() or "", f.nombre), reverse=True)
    _print_table(files, limit=None if args.limit == 0 else args.limit)
    print()
    print(f"{len(files)} files matched")
    return 0


def _cmd_info(args: argparse.Namespace) -> int:
    files = fetch_index()
    matches = [f for f in files if f.nombre == args.name]
    if not matches:
        # Try a substring match
        matches = [f for f in files if args.name.lower() in f.nombre.lower()]
    if not matches:
        print(f"no file matched {args.name!r}")
        return 1
    for f in matches[:5]:
        print(f"=== {f.nombre} ===")
        print(f"  category:  {f.categoria}")
        print(f"  path:      {f.path}")
        print(f"  date:      {f.date_iso()}")
        print(f"  state:     {f.estado}")
        print(f"  published: {f.fecha_publicacion}")
        print(f"  expires:   {f.vencimiento}")
        print(f"  description: {f.descripcion}")
        print(f"  presigned URL (truncated): {f.enlace_descarga[:120]}…")
        print()
    return 0


def _cmd_download(args: argparse.Namespace) -> int:
    files = fetch_index(force=args.refresh)
    if args.name:
        files = [f for f in files if f.nombre == args.name]
        if not files:
            print(f"no file matched {args.name!r}")
            return 1
    else:
        files = filter_files(
            files,
            name_pattern=args.pattern,
            category=args.category,
            since=args.since,
            until=args.until,
        )
    if not files:
        print("no files matched the filter")
        return 1
    print(f"will download {len(files)} files into {args.output}")
    failures = 0
    for f in files:
        try:
            download_one(
                f,
                output_dir=args.output,
                skip_existing=not args.force,
            )
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
            failures += 1
            _LOG.error("failed %s: %s", f.nombre, exc)
    print(f"  done: {len(files) - failures} ok, {failures} failed")
    return 1 if failures else 0


def _cmd_refresh(_args: argparse.Namespace) -> int:
    files = fetch_index(force=True)
    print(f"index refreshed — {len(files)} files cached at {DEFAULT_INDEX_PATH}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cen2gtopt.pcp_archive",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("-q", "--quiet", action="store_true")
    p.add_argument(
        "-V", "--version", action="version", version="cen2gtopt.pcp_archive 0.1.0"
    )

    sub = p.add_subparsers(dest="cmd")
    sub.required = True

    pl = sub.add_parser("list", help="List available files")
    pl.add_argument("--pattern", help="Regex on file name (case-insensitive).")
    pl.add_argument(
        "--category", choices=("PCP", "PID", "PMP", "PLP", "IM", "IA", "EC", "RE")
    )
    pl.add_argument("--since", help="YYYY-MM-DD (inclusive)")
    pl.add_argument("--until", help="YYYY-MM-DD (inclusive)")
    pl.add_argument(
        "--limit",
        type=int,
        default=30,
        help="Max rows to print (0 = no limit; default 30).",
    )
    pl.add_argument(
        "--refresh", action="store_true", help="Bypass the 6 h index cache."
    )
    pl.set_defaults(func=_cmd_list)

    pi = sub.add_parser("info", help="Show metadata for one file")
    pi.add_argument("name", help="File name (or substring).")
    pi.set_defaults(func=_cmd_info)

    pd_ = sub.add_parser("download", help="Download files matching a filter")
    pd_.add_argument("--name", help="Exact file name.")
    pd_.add_argument("--pattern", help="Regex on file name.")
    pd_.add_argument(
        "--category", choices=("PCP", "PID", "PMP", "PLP", "IM", "IA", "EC", "RE")
    )
    pd_.add_argument("--since", help="YYYY-MM-DD (inclusive)")
    pd_.add_argument("--until", help="YYYY-MM-DD (inclusive)")
    pd_.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_DOWNLOAD_ROOT,
        help=f"Download root (default: {DEFAULT_DOWNLOAD_ROOT})",
    )
    pd_.add_argument("--force", action="store_true", help="Overwrite existing files.")
    pd_.add_argument(
        "--refresh", action="store_true", help="Bypass the 6 h index cache."
    )
    pd_.set_defaults(func=_cmd_download)

    pr = sub.add_parser("refresh", help="Force-refresh the index cache")
    pr.set_defaults(func=_cmd_refresh)

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
