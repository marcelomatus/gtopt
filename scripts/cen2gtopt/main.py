# SPDX-License-Identifier: BSD-3-Clause
"""CLI entry point for cen2gtopt.

v1 is strictly an on-the-spot one-shot CLI per master plan §9.1.1.
No background loop, no recurring runner, no streaming append.
``--source sip`` is documented but raises NotImplementedError
until v1.1.
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys
from datetime import date as date_cls
from pathlib import Path
from typing import Optional


# Exit codes per master §9.5.2
EXIT_OK = 0
EXIT_PARTIAL = 2
EXIT_INPUT_ERROR = 3
EXIT_NETWORK_ERROR = 4


_LOG = logging.getLogger("cen2gtopt")


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cen2gtopt",
        description=(
            "Fetch CEN's published real-operation data on demand and "
            "write a canonical operation feed for gtopt-marginal-units. "
            "v1 is an on-the-spot one-shot CLI; see master plan §9."
        ),
    )
    p.add_argument("--start", required=True, help="Start date (YYYY-MM-DD).")
    p.add_argument("--end", required=True, help="End date (YYYY-MM-DD).")
    p.add_argument(
        "--out",
        required=True,
        type=Path,
        help="Output canonical-feed parquet directory.",
    )
    p.add_argument("--bus", help="Comma- or file-list of bus filters.")
    p.add_argument("--unit", help="Comma- or file-list of unit filters.")
    p.add_argument(
        "--source",
        choices=("sip", "csv", "auto"),
        default="auto",
        help="Data source: sip (v1.1), csv (v1 default), auto.",
    )
    p.add_argument("--api-key", help="SIP API key; falls back to $CEN_USER_KEY.")
    p.add_argument("--cache", type=Path, help="On-disk cache directory.")
    p.add_argument("--no-cache", action="store_true", help="Bypass cache and refetch.")
    p.add_argument(
        "--include",
        action="append",
        choices=(
            "dispatch",
            "demand",
            "lmp",
            "lmp_online",
            "flows",
            "emissions",
            "regimes",
            "fuel_costs",
        ),
        help=(
            "Which datasets to fetch (repeatable). Default: dispatch + demand "
            "+ lmp. Maps to SIP endpoints as: lmp → costo-marginal-real; "
            "lmp_online → costo-marginal-online; dispatch → generacion-real; "
            "demand → demanda-neta; regimes → instrucciones-operacionales-cmg "
            "+ limitaciones-transmision + programas-mantenimiento-mayor; "
            "fuel_costs → costo-combustible (master plan §4.11.1 driver #2)."
        ),
    )
    p.add_argument(
        "--source-tz",
        default="Chile/Continental",
        help="Source timezone (default Chile/Continental).",
    )
    p.add_argument(
        "--manifest-only",
        action="store_true",
        help="Write only manifest.json, no data (smoke test).",
    )
    p.add_argument(
        "--topology",
        type=Path,
        help="Topology JSON (CEN-format or gtopt planning JSON).",
    )
    p.add_argument("-v", "--verbose", action="count", default=0)
    p.add_argument("-q", "--quiet", action="store_true")
    return p


def cli(argv: Optional[list[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    _setup_logging(args.verbose, args.quiet)
    try:
        return _run(args)
    except _CliError as exc:
        _LOG.error("%s", exc)
        return exc.exit_code


class _CliError(Exception):
    def __init__(self, message: str, exit_code: int = EXIT_INPUT_ERROR) -> None:
        super().__init__(message)
        self.exit_code = exit_code


def _setup_logging(verbose: int, quiet: bool) -> None:
    level = logging.WARNING
    if quiet:
        level = logging.ERROR
    elif verbose >= 2:
        level = logging.DEBUG
    elif verbose == 1:
        level = logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )


def _run(args: argparse.Namespace) -> int:
    # Validate date order.
    try:
        start_date = date_cls.fromisoformat(args.start)
        end_date = date_cls.fromisoformat(args.end)
    except ValueError as exc:
        raise _CliError(f"invalid date format (expected YYYY-MM-DD): {exc}") from exc
    if start_date > end_date:
        raise _CliError(f"--start > --end ({args.start} > {args.end})")

    # Resolve source.
    api_key = (
        args.api_key or os.environ.get("CEN_USER_KEY") or os.environ.get("CEN_API_KEY")
    )
    if os.environ.get("CEN_API_KEY") and not os.environ.get("CEN_USER_KEY"):
        _LOG.warning("$CEN_API_KEY is the deprecated alias; set $CEN_USER_KEY instead.")
    source = args.source
    if source == "auto":
        source = "sip" if api_key else "csv"
    _LOG.info("auth=%s source=%s", "sip" if api_key else "csv-fallback", source)

    # Topology — load if provided so the JSON parses correctly. The
    # actual write (using the loaded topology) is gated behind
    # CSV-fetch logic below; v1 raises before reaching it.
    if args.topology is not None:
        from cen2gtopt._topology import load_topology  # noqa: PLC0415

        load_topology(args.topology)
    elif source == "csv":
        # CSV path requires a topology JSON (CEN doesn't publish topology
        # via CSV). Manifest-only mode is permitted without it.
        if not args.manifest_only:
            raise _CliError(
                "--source csv requires --topology pointing at a topology JSON "
                "(CEN-format or gtopt planning JSON)."
            )

    # SIP path — live as of 2026-05-06.
    if source == "sip" and not args.manifest_only:
        return _run_sip(args, api_key)

    # Manifest-only smoke test.
    if args.manifest_only:
        args.out.mkdir(parents=True, exist_ok=True)
        manifest = {
            "producer": "cen2gtopt",
            "producer_version": "1.0.0",
            "schema_version": "1.0.0",
            "manifest_only": True,
            "args": {
                "start": args.start,
                "end": args.end,
                "source": source,
                "include": list(args.include or ()),
            },
        }
        (args.out / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8"
        )
        _LOG.info("manifest-only: wrote %s", args.out / "manifest.json")
        return EXIT_OK

    # Production CSV path is v1; v1 ships parsers + writer but the
    # actual HTTP fetch + URL templates are TODO(verify-at-integration).
    # For v1 we expect callers to provide pre-fetched CSVs via the
    # cache directory. If --cache is empty, abort with a clear error.
    raise _CliError(
        "cen2gtopt v1 CSV-fetch path requires pre-populated cache files; "
        "live HTTP fetching is gated by TODO(verify-at-integration) URL "
        "templates in _csv_endpoints.py and is not exercised in v1. "
        "Either run --manifest-only or place CEN CSV exports in --cache "
        "before running. See master plan §9.2.1."
    )


def _run_sip(args: argparse.Namespace, api_key: str | None) -> int:
    """Fetch SIP data live and write a canonical operation feed.

    Implements master plan §9 Phase-2 path. Each ``--include`` keyword
    triggers one or more SIP endpoint fetches; results land under
    ``<out>/raw/<name>.parquet`` and are summarised in
    ``<out>/manifest.json:row_counts``.

    Endpoint dispatch:
      * ``lmp``        → costo-marginal-real
      * ``lmp_online`` → costo-marginal-online
      * ``dispatch``   → generacion-real
      * ``demand``     → demanda-neta
      * ``regimes``    → instrucciones-operacionales-cmg
                        + limitaciones-transmision
                        + programas-mantenimiento-mayor

    The ``centrales`` catalogue is always fetched (needed for any
    downstream analysis).
    """
    from cen2gtopt._cen_client import (  # noqa: PLC0415
        CenApiClient,
        CenApiConfig,
        CenAuthError,
        CenNotFoundError,
        CenServerError,
    )
    from cen2gtopt._sip_client import (  # noqa: PLC0415
        fetch_centrales,
        fetch_costo_combustible,
        fetch_costo_marginal_online,
        fetch_costo_marginal_real,
        fetch_demanda_neta,
        fetch_generacion_real,
        fetch_instrucciones_operacionales_cmg,
        fetch_limitaciones_transmision,
        fetch_programas_mantenimiento_mayor,
    )

    if api_key is None:
        raise _CliError(
            "SIP path requires a user_key. Set $CEN_USER_KEY in your "
            "environment or pass --api-key."
        )

    includes = set(args.include or ()) or {"dispatch", "demand", "lmp"}
    config = CenApiConfig(
        user_keys={"sip": api_key},
        verify_tls=False,  # CEN gateway uses non-system cert chain
    )

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    raw_dir = out / "raw"
    raw_dir.mkdir(exist_ok=True)

    # ``fetched`` accumulates {name → pandas DataFrame} for the manifest.
    fetched: dict = {}

    def _capture(name: str, fetch_callable, **kwargs) -> None:
        """Run a fetcher, log row-count, store result, tolerate
        CenNotFoundError / CenServerError on flaky upstreams."""
        try:
            df = fetch_callable(**kwargs)
        except CenNotFoundError as exc:
            _LOG.warning("SIP: %s — endpoint not yet routed: %s", name, exc)
            return
        except CenServerError as exc:
            _LOG.warning("SIP: %s — upstream error, skipping: %s", name, exc)
            return
        _LOG.info("SIP: %s — %d rows", name, len(df))
        fetched[name] = df

    try:
        with CenApiClient(config) as client:
            # Topology — centrales is always fetched; downstream consumers
            # need the catalogue regardless of which `--include` set was
            # chosen.
            _capture("centrales", fetch_centrales, client=client)

            # Marginal cost.
            if "lmp" in includes:
                _capture(
                    "costo_marginal_real",
                    fetch_costo_marginal_real,
                    client=client,
                    start=args.start,
                    end=args.end,
                )
            if "lmp_online" in includes:
                _capture(
                    "costo_marginal_online",
                    fetch_costo_marginal_online,
                    client=client,
                    start=args.start,
                    end=args.end,
                )

            # Dispatch.
            if "dispatch" in includes:
                _capture(
                    "generacion_real",
                    fetch_generacion_real,
                    client=client,
                    start=args.start,
                    end=args.end,
                )

            # Demand.
            if "demand" in includes:
                _capture(
                    "demanda_neta",
                    fetch_demanda_neta,
                    client=client,
                    start=args.start,
                    end=args.end,
                )

            # Fuel costs — declared MC per central (§4.11.1 driver #2).
            if "fuel_costs" in includes:
                _capture(
                    "costo_combustible",
                    fetch_costo_combustible,
                    client=client,
                    start=args.start,
                    end=args.end,
                )

            # Operational regimes (master plan §4.11.1 driver columns).
            if "regimes" in includes:
                _capture(
                    "instrucciones_operacionales_cmg",
                    fetch_instrucciones_operacionales_cmg,
                    client=client,
                    start=args.start,
                    end=args.end,
                )
                _capture(
                    "limitaciones_transmision",
                    fetch_limitaciones_transmision,
                    client=client,
                    start=args.start,
                    end=args.end,
                )
                _capture(
                    "programas_mantenimiento_mayor",
                    fetch_programas_mantenimiento_mayor,
                    client=client,
                    start=args.start,
                    end=args.end,
                )
    except CenAuthError as exc:
        raise _CliError(str(exc)) from exc
    except CenServerError as exc:
        raise _CliError(str(exc), exit_code=EXIT_NETWORK_ERROR) from exc

    # Write each fetched frame to <out>/raw/<name>.parquet.
    row_counts: dict[str, int] = {}
    for name, df in fetched.items():
        if df is None or df.empty:
            row_counts[name] = 0
            continue
        df.to_parquet(raw_dir / f"{name}.parquet", index=False)
        row_counts[name] = int(len(df))

    manifest = {
        "producer": "cen2gtopt",
        "producer_version": "1.0.0",
        "schema_version": "1.0.0",
        "source": "sip",
        "args": {
            "start": args.start,
            "end": args.end,
            "include": sorted(includes),
        },
        "row_counts": row_counts,
    }
    (out / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8"
    )
    _LOG.info("SIP: wrote %s — %d datasets", out, len(row_counts))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(cli())
