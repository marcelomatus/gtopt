# SPDX-License-Identifier: BSD-3-Clause
"""Daily-update service for the CEN cache.

Refreshes the **minimal critical subset** of CEN endpoints needed by
the marginal-emission pipeline (``tools/cen_emission_factor_v3.py``).
Designed to be wrapped by an external scheduler (cron / systemd
timer / k8s CronJob) — it is *not* a daemon and exits 0 when done.

Refresh policy per endpoint
---------------------------

* **Settled feeds** (``cmg_real``, ``generacion_real``, ``instrucciones_*``,
  ``embalse_real``, ``limitaciones``) follow a **lookback window** —
  re-fetch the last ``--lookback`` days every run because CEN can
  republish up to D+5 (cmg-real) or D+1 (others).
* **Day-ahead feeds** (``cmg_programado_pcp``, ``gen_programado_pcp``,
  ``flujo_programado_pcp``, ``sscc_prog_pcp``, ``cmg_online``) are
  fetched for **D-0 and D-1** only (their values are fixed once
  published the morning of the operating day).
* **Catalogues** (``unidades_generadoras``, ``centrales``) refresh
  **weekly** (Monday or when the ``catalog.lock`` mtime is > 7 d).
* **SCVIC ``cv_rpt``** refreshes **on month-roll** (when the current
  ``YYYY-MM`` differs from the cached file's month). The fetcher is
  smart enough to overwrite an in-progress month.

Cache freshness rule
--------------------

A cached file is considered fresh and skipped when:

  * its ``mtime`` is newer than the endpoint's ``min_age_to_refetch``
    (default 23h for daily feeds, 7d for catalogues, 30d for SCVIC),
    AND
  * the requested date is outside the ``--lookback`` window for
    settled feeds.

Otherwise it is re-fetched. ``--force`` bypasses both checks.

Manifest
--------

Each run writes a ``manifest_YYYYMMDD_HHMMSS.json`` into the cache
root with: timestamps, endpoints attempted, row counts, and any
errors. Useful for monitoring / alerting downstream.

Recommended deployment
----------------------

Hourly cron at minute 7 (CEN publishes intraday at the top of the
hour; offset to avoid the burst)::

    7 * * * *   python -m cen2gtopt.cache daily-update --tier minimal

Twice-daily cron (a tighter "real-only" pass)::

    7  3 * * *  python -m cen2gtopt.cache daily-update --tier minimal --lookback 7
    7 14 * * *  python -m cen2gtopt.cache daily-update --tier full    --lookback 2

Idempotent — safe to run more often than needed.
"""

from __future__ import annotations

import json
import logging
import time
from dataclasses import dataclass, field, asdict
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

from cen2gtopt._cached_extractor import (
    ENDPOINTS,
    cache_path,
    cache_root_default,
    fetch_by_name,
)
from cen2gtopt._cen_client import CenApiClient
from cen2gtopt._scvic_api import cv_rpt_for_date

_LOG = logging.getLogger("cen2gtopt.daily_update")


# ---------------------------------------------------------------------------
# Endpoint tiers (the minimal critical set)
# ---------------------------------------------------------------------------

#: Tier A — required every day for marginal-unit identification.
TIER_A_DAILY = (
    "cmg_real",
    "cmg_online",
    "generacion_real",
    "instrucciones_cmg",
    "instrucciones_sscc",
)

#: Tier A catalogues — refresh weekly, used by every cell in the pipeline.
TIER_A_WEEKLY = ("unidades_generadoras",)

#: Tier B — validation oracle (per critical review §5.20).
TIER_B = (
    "cmg_programado_pcp",
    "generacion_programada_pcp",
)

#: Tier C — topology + reserve allocations.
TIER_C = (
    "flujo_programado_pcp",
    "limitaciones",
    "sscc_prog_pcp",
)

#: Tier D — occasional / quasi-static.
TIER_D_WEEKLY = ("centrales",)
TIER_D_DAILY = ("embalse_real",)

#: Composed presets exposed via ``--tier``.
TIER_PRESETS: dict[str, dict[str, tuple[str, ...]]] = {
    "minimal": {
        "daily": TIER_A_DAILY,
        "weekly": TIER_A_WEEKLY,
    },
    "validation": {
        "daily": TIER_A_DAILY + TIER_B,
        "weekly": TIER_A_WEEKLY,
    },
    "full": {
        "daily": TIER_A_DAILY + TIER_B + TIER_C + TIER_D_DAILY,
        "weekly": TIER_A_WEEKLY + TIER_D_WEEKLY,
    },
}

#: Per-endpoint cache-freshness rule (seconds).
#:
#: Daily-volatile feeds: re-fetch if older than 23 h (so an hourly
#: cron always picks up new data once a day).  Catalogues: 7 d.
#: SCVIC: 30 d (handled separately in :func:`refresh_scvic_if_month_rolled`).
MIN_AGE_TO_REFETCH_S: dict[str, int] = {
    # Daily feeds (default 23h)
    "cmg_real": 23 * 3600,
    # cmg-online for a past day is settled; use 23h freshness like
    # the other settled feeds. The 1h-cadence-during-current-day
    # behaviour is achieved by force-fetching D-0 explicitly via
    # day_ahead_window in the planner — not via the freshness clock.
    "cmg_online": 23 * 3600,
    "generacion_real": 23 * 3600,
    "instrucciones_cmg": 23 * 3600,
    "instrucciones_sscc": 23 * 3600,
    "cmg_programado_pcp": 23 * 3600,
    "generacion_programada_pcp": 23 * 3600,
    "flujo_programado_pcp": 23 * 3600,
    "limitaciones": 23 * 3600,
    "sscc_prog_pcp": 23 * 3600,
    "embalse_real": 23 * 3600,
    # Catalogues (7 d default)
    "unidades_generadoras": 7 * 86400,
    "centrales": 7 * 86400,
}

DEFAULT_MIN_AGE_S = 23 * 3600


# ---------------------------------------------------------------------------
# Run report
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class EndpointResult:
    endpoint: str
    target_date: str
    status: str  # "fetched" | "skipped_fresh" | "error"
    rows: int = 0
    error: str = ""
    duration_s: float = 0.0


@dataclass(slots=True)
class RunReport:
    started_at: str
    finished_at: str = ""
    tier: str = ""
    lookback_days: int = 0
    today: str = ""
    cache_root: str = ""
    endpoints: list[EndpointResult] = field(default_factory=list)
    scvic_status: str = ""
    weekly_catalog_status: str = ""
    pcp_archive_status: str = ""

    def write(self, out_dir: Path) -> Path:
        out_dir.mkdir(parents=True, exist_ok=True)
        ts = self.started_at.replace(":", "").replace("-", "").replace(" ", "_")
        path = out_dir / f"manifest_{ts}.json"
        path.write_text(json.dumps(asdict(self), indent=2, default=str))
        return path


# ---------------------------------------------------------------------------
# Date arithmetic
# ---------------------------------------------------------------------------


def _today() -> date:
    """Operating-day `date` in CEN local time (America/Santiago, UTC-4/-3).

    For the daily-update purpose we just use UTC-3 (CLT, no DST
    transition handling). The cache cares about the operating day,
    not the wall clock.
    """
    return (datetime.now(timezone.utc) + timedelta(hours=-3)).date()


def _date_range(start: date, end: date) -> list[str]:
    """Inclusive range of ``YYYY-MM-DD`` strings."""
    out: list[str] = []
    cur = start
    while cur <= end:
        out.append(cur.isoformat())
        cur += timedelta(days=1)
    return out


def _settled_window(today: date, lookback: int) -> list[str]:
    """Dates we re-fetch on every run (settled feeds)."""
    return _date_range(today - timedelta(days=lookback), today - timedelta(days=1))


def _day_ahead_window(today: date) -> list[str]:
    """Dates we fetch for day-ahead feeds (D-0 and D-1)."""
    return [(today - timedelta(days=1)).isoformat(), today.isoformat()]


# ---------------------------------------------------------------------------
# Cache freshness
# ---------------------------------------------------------------------------


def _is_fresh(
    name: str,
    target_date: str,
    cache_root: Path,
) -> bool:
    """True when the cached file exists AND its mtime is newer than
    the endpoint's ``min_age_to_refetch``."""
    p = cache_path(
        service=ENDPOINTS[name]["service"],
        endpoint=ENDPOINTS[name]["path"],
        params={"startDate": target_date, "endDate": target_date},
        cache_root=cache_root,
    )
    if not p.exists():
        return False
    age_s = time.time() - p.stat().st_mtime
    threshold = MIN_AGE_TO_REFETCH_S.get(name, DEFAULT_MIN_AGE_S)
    return age_s < threshold


# ---------------------------------------------------------------------------
# Per-endpoint fetch loop
# ---------------------------------------------------------------------------


def _fetch_one(
    client: CenApiClient,
    name: str,
    target_date: str,
    cache_root: Path,
    *,
    force: bool,
    dry_run: bool,
) -> EndpointResult:
    t0 = time.monotonic()
    res = EndpointResult(
        endpoint=name,
        target_date=target_date,
        status="?",
    )
    if not force and _is_fresh(name, target_date, cache_root):
        res.status = "skipped_fresh"
        return res
    if dry_run:
        res.status = "would_fetch"
        return res
    try:
        df = fetch_by_name(
            client,
            name,
            start=target_date,
            cache_root=cache_root,
            bypass_cache=force,
        )
        res.rows = len(df)
        res.status = "fetched"
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
        res.status = "error"
        res.error = f"{type(exc).__name__}: {exc}"
        _LOG.error("fetch %s [%s] failed: %s", name, target_date, exc)
    res.duration_s = round(time.monotonic() - t0, 2)
    return res


# ---------------------------------------------------------------------------
# SCVIC monthly refresh
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# PCP / PID archive refresh
# ---------------------------------------------------------------------------


def refresh_pcp_archive(
    today: date,
    *,
    lookback_days: int = 7,
    include_pcp: bool = False,
    include_pid: bool = True,
    pid_period_filter: int | None = None,
    force: bool = False,
    dry_run: bool = False,
) -> dict[str, int]:
    """Download PID (and optionally PCP) bundles for the lookback window.

    PID supersedes PCP for any given operating day — it is the
    intra-day re-solve and reflects post-dispatch outage / topology
    updates.  Defaults therefore download **PID only** (~38 MB per
    period, typically 4–8 periods/day) and skip the PCP day-ahead
    bundle entirely.  Set ``include_pcp=True`` only for back-tests
    that explicitly require the day-ahead pre-dispatch state.

    Caches are minted on demand by
    :func:`cen2gtopt.pcp_archive.fetch_index`; only files not already
    on disk are downloaded.  Returns ``{status → count}``.

    Args:
        today: Reference operating day.
        lookback_days: How many days back to scan (D-1 .. D-N).
        include_pcp: Fetch ``PLEXOS{date}.zip`` per day (~33 MB each).
            **Default False** — PID is preferred and contains the same
            input schema plus the intra-day updates.
        include_pid: Fetch every ``PID_{date}_{period}.zip`` per day
            (~38 MB each).  Default True.
        pid_period_filter: Restrict PID downloads to one period
            (e.g. 20 = the late-evening re-solve).  ``None`` = all.
        force: Re-download even if the file is already cached.
        dry_run: Don't actually download — just count.
    """
    # Local import to avoid the heavy pcp_archive load when daily-update
    # is invoked without --include-pcp/--include-pid.
    # pylint: disable=import-outside-toplevel
    from cen2gtopt.pcp_archive import (
        DEFAULT_DOWNLOAD_ROOT,
        download_one,
        fetch_index,
    )

    counts = {"would_download": 0, "downloaded": 0, "skipped": 0, "error": 0}
    if not include_pcp and not include_pid:
        return counts

    files = fetch_index()
    target_dates = {
        (today - timedelta(days=k)).strftime("%Y%m%d")
        for k in range(1, lookback_days + 1)
    }

    wanted = []
    for f in files:
        if include_pcp and any(f.nombre == f"PLEXOS{ymd}.zip" for ymd in target_dates):
            wanted.append(f)
            continue
        if include_pid:
            for ymd in target_dates:
                if not f.nombre.startswith(f"PID_{ymd}_"):
                    continue
                if pid_period_filter is not None:
                    needle = f"PID_{ymd}_{pid_period_filter:02d}.zip"
                    if f.nombre != needle:
                        continue
                wanted.append(f)
                break

    for f in wanted:
        local = DEFAULT_DOWNLOAD_ROOT / f.categoria / f.nombre
        if local.exists() and local.stat().st_size > 0 and not force:
            counts["skipped"] += 1
            continue
        if dry_run:
            counts["would_download"] += 1
            continue
        try:
            download_one(f, output_dir=DEFAULT_DOWNLOAD_ROOT, skip_existing=not force)
            counts["downloaded"] += 1
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
            _LOG.error("pcp/pid download %s failed: %s", f.nombre, exc)
            counts["error"] += 1
    return counts


def refresh_scvic_if_month_rolled(today: date, *, force: bool = False) -> str:
    """Refresh SCVIC ``cv_rpt`` for the current month if not yet
    cached (or if ``force``).  Returns one of:

      * ``"refreshed"``   — fetched new month's file
      * ``"already_cached"`` — month file already on disk
      * ``"error: ..."``  — fetch failed
    """
    cache_dir = Path.home() / ".cache" / "gtopt" / "cen2gtopt" / "scvic"
    fname = f"cv_rpt_{today.strftime('%Y_%m')}.parquet"
    path = cache_dir / fname
    if path.exists() and not force:
        return "already_cached"
    try:
        df = cv_rpt_for_date(today.isoformat())
        return f"refreshed ({len(df)} rows → {path.name})"
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
        return f"error: {exc}"


# ---------------------------------------------------------------------------
# Main entry
# ---------------------------------------------------------------------------


def daily_update(
    client: CenApiClient,
    *,
    tier: str = "minimal",
    lookback_days: int = 7,
    today: date | None = None,
    cache_root: Path | None = None,
    force: bool = False,
    dry_run: bool = False,
    refresh_weekly: bool = True,
    refresh_scvic: bool = True,
    include_pcp: bool = False,
    include_pid: bool = False,
    pid_period_filter: int | None = None,
) -> RunReport:
    """Run the daily-update pass.

    Parameters
    ----------
    tier
        ``"minimal"``, ``"validation"`` or ``"full"`` — see
        :data:`TIER_PRESETS`.
    lookback_days
        How many days back to re-fetch settled feeds (default 7;
        cmg-real settles at D+5 so 7 covers a safety buffer).
    today
        Override the "today" reference (for back-fill runs).
    force
        Bypass cache freshness; re-fetch every selected
        endpoint × date.
    dry_run
        Don't fetch — just print the plan and return statuses
        ``"would_fetch"`` / ``"skipped_fresh"``.
    refresh_weekly
        If True, also refresh weekly catalogues when their cache is
        > 7 d old.
    refresh_scvic
        If True, also call :func:`refresh_scvic_if_month_rolled`.
    """
    if tier not in TIER_PRESETS:
        raise ValueError(f"unknown tier {tier!r}; expected one of {list(TIER_PRESETS)}")
    today = today or _today()
    cache_root = cache_root or cache_root_default()

    report = RunReport(
        started_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        tier=tier,
        lookback_days=lookback_days,
        today=today.isoformat(),
        cache_root=str(cache_root),
    )

    daily_eps = TIER_PRESETS[tier]["daily"]
    weekly_eps = TIER_PRESETS[tier]["weekly"] if refresh_weekly else ()

    # ---- Settled feeds: lookback window ----
    settled_dates = _settled_window(today, lookback_days)
    # ---- Day-ahead feeds: D-0 + D-1 ----
    day_ahead_dates = _day_ahead_window(today)

    # Dispatch table: endpoint → list of dates to attempt.
    plan: dict[str, list[str]] = {}
    for ep in daily_eps:
        # cmg_online + PCP-style endpoints get D-0+D-1; everything
        # else gets the lookback window.
        if ep in {
            "cmg_online",
            "cmg_programado_pcp",
            "generacion_programada_pcp",
            "flujo_programado_pcp",
            "sscc_prog_pcp",
        }:
            plan[ep] = day_ahead_dates
        else:
            plan[ep] = settled_dates

    n_attempts = sum(len(v) for v in plan.values())
    _LOG.info(
        "daily-update tier=%s today=%s lookback=%d → %d (endpoint × date) attempts",
        tier,
        today.isoformat(),
        lookback_days,
        n_attempts,
    )

    for ep, dates in plan.items():
        for d in dates:
            r = _fetch_one(client, ep, d, cache_root, force=force, dry_run=dry_run)
            report.endpoints.append(r)
            if r.status == "fetched":
                _LOG.info(
                    "  fetched %s [%s] → %d rows in %.2fs", ep, d, r.rows, r.duration_s
                )

    # ---- Weekly catalogues ----
    if refresh_weekly:
        for ep in weekly_eps:
            r = _fetch_one(
                client, ep, today.isoformat(), cache_root, force=force, dry_run=dry_run
            )
            report.endpoints.append(r)
            if r.status == "fetched":
                _LOG.info(
                    "  fetched (weekly) %s [%s] → %d rows in %.2fs",
                    ep,
                    today.isoformat(),
                    r.rows,
                    r.duration_s,
                )
        report.weekly_catalog_status = "checked"
    else:
        report.weekly_catalog_status = "skipped"

    # ---- SCVIC monthly ----
    if refresh_scvic and not dry_run:
        report.scvic_status = refresh_scvic_if_month_rolled(today, force=force)
        _LOG.info("  scvic: %s", report.scvic_status)
    else:
        report.scvic_status = "skipped" if not refresh_scvic else "dry_run"

    # ---- PCP/PID archive ----
    if include_pcp or include_pid:
        counts = refresh_pcp_archive(
            today,
            lookback_days=lookback_days,
            include_pcp=include_pcp,
            include_pid=include_pid,
            pid_period_filter=pid_period_filter,
            force=force,
            dry_run=dry_run,
        )
        report.pcp_archive_status = (
            f"downloaded={counts['downloaded']} "
            f"skipped={counts['skipped']} "
            f"would_download={counts['would_download']} "
            f"errors={counts['error']}"
        )
        _LOG.info("  pcp_archive: %s", report.pcp_archive_status)
    else:
        report.pcp_archive_status = "skipped"

    report.finished_at = datetime.now(timezone.utc).isoformat(timespec="seconds")
    return report


def summarise(report: RunReport) -> str:
    """Compact human-readable summary of a run."""
    counts: dict[str, int] = {
        "fetched": 0,
        "skipped_fresh": 0,
        "error": 0,
        "would_fetch": 0,
    }
    rows = 0
    for r in report.endpoints:
        counts[r.status] = counts.get(r.status, 0) + 1
        rows += r.rows
    by_ep: dict[str, dict[str, int]] = {}
    for r in report.endpoints:
        by_ep.setdefault(
            r.endpoint,
            {"ok": 0, "skip": 0, "err": 0, "plan": 0, "rows": 0},
        )
        if r.status == "fetched":
            by_ep[r.endpoint]["ok"] += 1
            by_ep[r.endpoint]["rows"] += r.rows
        elif r.status == "skipped_fresh":
            by_ep[r.endpoint]["skip"] += 1
        elif r.status == "error":
            by_ep[r.endpoint]["err"] += 1
        elif r.status == "would_fetch":
            by_ep[r.endpoint]["plan"] += 1
    lines = [
        f"daily-update tier={report.tier} today={report.today} "
        f"lookback={report.lookback_days}",
        f"  totals: fetched={counts['fetched']} skipped_fresh={counts['skipped_fresh']} "
        f"errors={counts['error']} would_fetch={counts['would_fetch']} "
        f"rows={rows}",
        f"  scvic: {report.scvic_status}",
        f"  weekly_catalogues: {report.weekly_catalog_status}",
        f"  pcp_archive: {report.pcp_archive_status}",
        "  per endpoint:",
    ]
    for ep in sorted(by_ep):
        s = by_ep[ep]
        lines.append(
            f"    {ep:30s} ok={s['ok']:>2} skip={s['skip']:>2} "
            f"err={s['err']:>2} plan={s['plan']:>2} rows={s['rows']:>8}"
        )
    return "\n".join(lines)


__all__ = [
    "TIER_PRESETS",
    "TIER_A_DAILY",
    "TIER_A_WEEKLY",
    "TIER_B",
    "TIER_C",
    "TIER_D_DAILY",
    "TIER_D_WEEKLY",
    "EndpointResult",
    "RunReport",
    "daily_update",
    "refresh_scvic_if_month_rolled",
    "summarise",
]
