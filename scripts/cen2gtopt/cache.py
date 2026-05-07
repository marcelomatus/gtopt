# SPDX-License-Identifier: BSD-3-Clause
"""``python -m cen2gtopt.cache`` — pre-warm or inspect the on-disk cache.

Examples
--------

Fetch every known endpoint for one date::

    python -m cen2gtopt.cache fetch --date 2026-04-22

Show what's cached::

    python -m cen2gtopt.cache list

Clear one endpoint::

    python -m cen2gtopt.cache clear --endpoint unidades_generadoras
"""

from __future__ import annotations

import argparse
import logging
import os
import sys

from cen2gtopt._cached_extractor import (
    ENDPOINTS,
    cache_root_default,
    clear_cache,
    prefetch_date,
)
from cen2gtopt._cen_client import CenApiClient, CenApiConfig
from cen2gtopt.daily_update import (
    TIER_PRESETS,
    daily_update,
    summarise,
)


def _make_client() -> CenApiClient:
    keys = {
        s: os.environ[k]
        for k, s in [
            ("CEN_USER_KEY", "sip"),
            ("CEN_OPERACION_KEY", "operacion"),
            ("CEN_MERCADOS_KEY", "mercados"),
            ("CEN_PLANIFICACION_KEY", "planificacion"),
        ]
        if k in os.environ
    }
    if "sip" not in keys:
        # Hard-coded default for convenience (the user's known SIP key).
        keys["sip"] = "492a5028ad67fd63791e28591cd29c01"
    cfg = CenApiConfig(user_keys=keys, verify_tls=False)
    return CenApiClient(cfg)


def _cmd_fetch(args: argparse.Namespace) -> int:
    client = _make_client()
    targets = args.endpoints or list(ENDPOINTS.keys())
    print(f"prefetching {len(targets)} endpoint(s) for {args.date} …")
    counts = prefetch_date(
        client,
        date=args.date,
        names=targets,
        bypass_cache=args.no_cache,
    )
    print()
    print(f"{'endpoint':30s} rows")
    print("-" * 45)
    for n, cnt in counts.items():
        marker = "✓" if cnt > 0 else ("∅" if cnt == 0 else "✗")
        print(f"{n:30s} {cnt:>8} {marker}")
    return 0


def _cmd_list(_args: argparse.Namespace) -> int:
    root = cache_root_default()
    if not root.exists():
        print(f"cache empty: {root}")
        return 0
    files = sorted(root.rglob("*.parquet"))
    print(f"{root}: {len(files)} cached frame(s)")
    for p in files:
        size = p.stat().st_size
        rel = p.relative_to(root)
        print(f"  {size:>10d}b  {rel}")
    return 0


def _cmd_clear(args: argparse.Namespace) -> int:
    n = clear_cache(service=args.service, endpoint=args.endpoint)
    print(f"removed {n} cached file(s)")
    return 0


def _cmd_daily_update(args: argparse.Namespace) -> int:
    client = _make_client()
    today = None
    if args.today:
        from datetime import date as _date

        today = _date.fromisoformat(args.today)
    report = daily_update(
        client,
        tier=args.tier,
        lookback_days=args.lookback,
        today=today,
        force=args.force,
        dry_run=args.dry_run,
        refresh_weekly=not args.skip_weekly,
        refresh_scvic=not args.skip_scvic,
        include_pcp=args.include_pcp,
        include_pid=args.include_pid,
        pid_period_filter=args.pid_period,
    )
    print(summarise(report))
    if not args.dry_run:
        manifest_dir = cache_root_default() / "_manifests"
        path = report.write(manifest_dir)
        print(f"\nmanifest: {path}")
    n_err = sum(1 for r in report.endpoints if r.status == "error")
    return 1 if n_err else 0


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    sub = p.add_subparsers(dest="cmd")
    sub.required = True

    f = sub.add_parser("fetch", help="Prefetch one or more endpoints")
    f.add_argument("--date", required=True, help="YYYY-MM-DD")
    f.add_argument("--endpoints", nargs="*", help=f"Subset of: {', '.join(ENDPOINTS)}")
    f.add_argument(
        "--no-cache",
        action="store_true",
        help="Bypass the parquet cache (re-fetch and overwrite)",
    )
    f.set_defaults(func=_cmd_fetch)

    lst = sub.add_parser("list", help="List cached files")
    lst.set_defaults(func=_cmd_list)

    c = sub.add_parser("clear", help="Remove cached files")
    c.add_argument("--service", help="Limit to a single service (e.g. sip)")
    c.add_argument("--endpoint", help="Endpoint short name (e.g. unidades_generadoras)")
    c.set_defaults(func=_cmd_clear)

    d = sub.add_parser(
        "daily-update",
        help="Refresh the minimal critical CEN cache "
        "(designed to be wrapped by cron / systemd timer).",
        description=(
            "Fetches the Tier-A subset of CEN endpoints required by the\n"
            "marginal-emission pipeline. Settled feeds are re-fetched\n"
            "across a lookback window (default 7 days) to catch CEN's\n"
            "D+5 settlement revisions; day-ahead PCP feeds are fetched\n"
            "for D-0 + D-1 only.  Idempotent — safe to run hourly."
        ),
        formatter_class=argparse.RawTextHelpFormatter,
    )
    d.add_argument(
        "--tier",
        choices=tuple(TIER_PRESETS.keys()),
        default="minimal",
        help="Endpoint tier (default: minimal). "
        "minimal=cmg+gen+instr, validation=+PCP oracle, "
        "full=+topology+reserves",
    )
    d.add_argument(
        "--lookback",
        type=int,
        default=7,
        help="Days back to re-fetch settled feeds (default 7).",
    )
    d.add_argument("--today", help="Override 'today' (YYYY-MM-DD) for back-fill runs.")
    d.add_argument(
        "--force",
        action="store_true",
        help="Bypass cache freshness; re-fetch everything.",
    )
    d.add_argument("--dry-run", action="store_true", help="Show plan without fetching.")
    d.add_argument(
        "--skip-weekly",
        action="store_true",
        help="Skip the weekly catalogue refresh (unidades_generadoras, centrales).",
    )
    d.add_argument(
        "--skip-scvic",
        action="store_true",
        help="Skip the SCVIC monthly cv-rpt refresh.",
    )
    d.add_argument(
        "--include-pcp",
        action="store_true",
        help="Also download PLEXOS{date}.zip (PCP day-ahead bundle, "
        "~33 MB/day) into the pcp_archive cache for the lookback "
        "window. Required by `cen2gtopt.pcp_inputs --source pcp`.",
    )
    d.add_argument(
        "--include-pid",
        action="store_true",
        help="Also download every PID_{date}_{period}.zip "
        "(intra-day re-solve, ~38 MB each, multiple per day) for "
        "the lookback window. Use --pid-period to restrict to one. "
        "Required by `cen2gtopt.pcp_inputs --source pid`.",
    )
    d.add_argument(
        "--pid-period",
        type=int,
        metavar="PP",
        help="When --include-pid is set, only fetch the PID re-solve "
        "for this period (1-24).  Default: all periods.",
    )
    d.set_defaults(func=_cmd_daily_update)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
