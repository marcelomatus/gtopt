#!/usr/bin/env python3
"""gtopt_convergence_table — pretty-print a cascade SDDP convergence table.

Primary source: ``results/logs/gtopt*.log`` (the gtopt run log).  Each SDDP
iteration writes two lines:

    ─── iter N (async) ───  elapsed= XXs  [ETA= YYm] [CONVERGED]
        iter N  obj    UB=...G  LB=...G  gap=±NN%  Δgap=±NN%  kappa=...

And each cascade level is bracketed by:

    Cascade [<name>]: new solver (max_iters=..., apertures=..., tol=...)
    ...
    Cascade [<name>]: N iters, LB=..., UB=..., gap=..., new_cuts=K, T.Ts (converged)
    [DIAG] Cascade [<name>]: transition decision: next_level.transition.has_value=true

These lines persist for the lifetime of the log file, so the multi-level
history survives across level transitions (unlike ``solver_status.json``
which is a rolling single-snapshot).  When the log is absent (very early
in a fresh run, or under a launcher that swallowed stdout), the script
falls back to:

  * ``results/solver_status.json`` for the live iter's UB/LB/gap, and
  * ``results/cuts/<level>/sddp_cuts_*.parquet`` mtimes for wall times.

  Outputs the multi-level table format used by the gtopt session monitor::

      ═══════════════════════════════════════════════════════════════════════
      LEVEL: warmup    max_iters=20  apertures=1  tol=0.001  ...
      ───────────────────────────────────────────────────────────────────────
      iter   UB     LB    gap   Δgap   kappa   iter_wall  level_wall  total
      ───────────────────────────────────────────────────────────────────────
        0   3.531G 18.45M +99.48% +100% 3.60e+06   33.5s    33.5s    33.5s
        ...
        12  2.874G 1.822G +36.60% +0.01% 1.14e+09 14.2s  3m 12.6s 3m 12.6s [CONVERGED]
      ─── 13 iters, level total 190.8s, new_cuts=10400 → 10463 serialised

Caveats
-------

* ``solver_status.json`` rotates its history — older iterations may be
  pruned.  Rows for missing iters show "—" in UB/LB/gap.
* ``kappa`` and the canonical ΔUB lookback aren't in solver_status; we
  derive a simple iter-to-iter relative ΔUB as a proxy, and leave kappa
  blank unless the gtopt log file is reachable (auto-detect under
  ``results/logs/gtopt_*.log``).
* The ``[CONVERGED]`` marker uses the per-row ``converged`` flag in the
  history entry; it does not annotate iters whose entries have rolled
  off the history window.

Usage
-----

    python tools/gtopt_convergence_table.py <case_dir>
    python tools/gtopt_convergence_table.py /path/to/run --json input.json

Where ``case_dir`` contains both the input JSON and the ``results/``
output directory (the default layout for ``plp2gtopt`` → ``gtopt``).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

CUT_FILE_RE = re.compile(r"^sddp_cuts_(\d+)\.parquet$")
LOG_FILE_RE = re.compile(r"^gtopt_\d+\.log$")


# ─── Formatting helpers ──────────────────────────────────────────────────────


def fmt_money(x: float | None) -> str:
    """Format a money-like number as e.g. ``2.874G`` / ``18.45M`` / ``42.1k``."""
    if x is None:
        return "—"
    a = abs(x)
    if a >= 1e9:
        return f"{x / 1e9:.3f}G"
    if a >= 1e6:
        return f"{x / 1e6:.2f}M"
    if a >= 1e3:
        return f"{x / 1e3:.2f}k"
    return f"{x:.2f}"


def fmt_pct(x: float | None) -> str:
    """Format a fraction as ``+12.3456%`` (signed, 4dp).

    4 decimals to match gtopt's log emit precision (``{:+8.4f}%`` in
    ``log_format.hpp``) and to distinguish Δgap values at the
    cascade's tightest tolerance bands (e.g. L0=0.01 %).  2dp rounded
    every sub-0.005 % Δgap to "+0.00 %", indistinguishable from a
    rounded-zero proxy.  4dp preserves the real value to ``0.0001 %``,
    which is exactly the L0 stationary_tol floor.
    """
    if x is None:
        return "—"
    return f"{x:+.4%}"


def fmt_wall(s: float) -> str:
    """Format seconds as ``33.5s`` / ``1m 23.5s`` / ``1h 23m 45s``."""
    if s < 60:
        return f"{s:5.1f}s"
    if s < 3600:
        m = int(s // 60)
        r = s - 60 * m
        return f"{m}m {r:04.1f}s"
    h = int(s // 3600)
    r = s - 3600 * h
    m = int(r // 60)
    rr = r - 60 * m
    return f"{h}h {m:02d}m {rr:04.1f}s"


def fmt_kappa(k: float | None) -> str:
    if k is None:
        return "—"
    return f"{k:.2e}"


# ─── Data shapes ─────────────────────────────────────────────────────────────


@dataclass
class LevelConfig:
    name: str
    max_iterations: int | None = None
    num_apertures: int | None = None
    convergence_tol: float | None = None
    stationary_tol: float | None = None
    stationary_gap_ceiling: float | None = None


@dataclass
class IterRow:
    level: str
    iter: int
    mtime: float
    upper_bound: float | None = None
    lower_bound: float | None = None
    gap: float | None = None
    # gtopt's actual internal Δgap (parsed from the log's
    # ``Δgap=±NN%`` field).  This is the WINDOWED stationary metric
    # the SDDP convergence check evaluates against ``stationary_tol``
    # — distinct from the per-iter ``|ΔUB|/|UB|`` proxy that
    # ``render_table`` falls back to when no log is reachable.  The
    # two can disagree substantially (the log value uses a multi-iter
    # lookback to filter sample noise), so this field, when present,
    # takes precedence.  ``None`` means the row's log line didn't
    # carry a Δgap clause (e.g. iter 0 with no lookback).
    delta_gap: float | None = None
    converged: bool = False
    cuts_added: int | None = None
    kappa: float | None = None


@dataclass
class LevelStats:
    cfg: LevelConfig
    rows: list[IterRow] = field(default_factory=list)
    inherited_cuts: int | None = None  # populated from previous level's tail
    new_cuts_total: int = 0


# ─── Discovery ───────────────────────────────────────────────────────────────


def find_input_json(case_dir: Path) -> Path:
    """Locate the cascade input JSON in `case_dir`.

    Looks for any top-level ``*.json`` that isn't the planning output or
    the solver status.  Skips ``results/`` recursively.
    """
    candidates = [
        p
        for p in case_dir.glob("*.json")
        if p.name not in {"planning.json", "solver_status.json"}
    ]
    if not candidates:
        sys.exit(
            f"could not locate input JSON in {case_dir} (expected a top-level *.json)"
        )
    # Prefer the largest file (the case JSON is bigger than ancillary files).
    candidates.sort(key=lambda p: p.stat().st_size, reverse=True)
    return candidates[0]


def parse_level_configs(input_json: Path) -> list[LevelConfig]:
    """Parse cascade level configs from the input JSON.

    Falls back to a single anonymous level when the JSON is plain SDDP (no
    `cascade_options.level_array`).
    """
    with input_json.open() as f:
        cfg = json.load(f)

    cascade = cfg.get("options", {}).get("cascade_options", {}) or cfg.get(
        "cascade_options", {}
    )
    level_array = cascade.get("level_array")
    if not level_array:
        # Non-cascade run — synthesise one level named "sddp".
        sddp = cfg.get("options", {}).get("sddp_options", {}) or cfg.get(
            "sddp_options", {}
        )
        return [
            LevelConfig(
                name="sddp",
                max_iterations=sddp.get("max_iterations"),
                num_apertures=sddp.get("num_apertures"),
                convergence_tol=sddp.get("convergence_tol"),
                stationary_tol=sddp.get("stationary_tol"),
                stationary_gap_ceiling=sddp.get("stationary_gap_ceiling"),
            )
        ]

    out: list[LevelConfig] = []
    for lvl in level_array:
        sddp = lvl.get("sddp_options") or {}
        out.append(
            LevelConfig(
                name=lvl.get("name", f"L{len(out)}"),
                max_iterations=sddp.get("max_iterations"),
                num_apertures=sddp.get("num_apertures"),
                convergence_tol=sddp.get("convergence_tol"),
                stationary_tol=sddp.get("stationary_tol"),
                stationary_gap_ceiling=sddp.get("stationary_gap_ceiling"),
            )
        )
    return out


def collect_iter_rows(
    cuts_dir: Path, *, min_mtime: float | None = None
) -> list[IterRow]:
    """Walk ``cuts/<level>/sddp_cuts_*.parquet`` to anchor per-iter mtimes.

    When ``min_mtime`` is supplied, cut files older than that timestamp
    are excluded.  Used to filter out artifacts from prior runs that
    overlapped the same ``cuts/`` directory: the SDDP solver only
    overwrites cut files for iter numbers it actually reaches in the
    current run, so iters > current_iter persist as stale leftovers
    until the cuts dir is cleared.  Without this filter the table
    would mix mtimes from two different runs and produce nonsensical
    per-iter wall times.
    """
    rows: list[IterRow] = []
    if not cuts_dir.is_dir():
        return rows
    for level_dir in sorted(cuts_dir.iterdir()):
        if not level_dir.is_dir():
            continue
        for f in level_dir.glob("sddp_cuts_*.parquet"):
            m = CUT_FILE_RE.match(f.name)
            if not m:
                continue
            mtime = f.stat().st_mtime
            if min_mtime is not None and mtime < min_mtime:
                continue
            rows.append(
                IterRow(
                    level=level_dir.name,
                    iter=int(m.group(1)),
                    mtime=mtime,
                )
            )
    rows.sort(key=lambda r: r.mtime)
    return rows


def merge_history(rows: list[IterRow], solver_status: Path) -> dict:
    """Layer UB/LB/gap from solver_status.json into matching rows.

    Returns the parsed solver_status dict for downstream use.
    """
    if not solver_status.is_file():
        return {}
    with solver_status.open() as f:
        s = json.load(f)
    by_iter = {h["iteration"]: h for h in s.get("history", [])}
    for r in rows:
        h = by_iter.get(r.iter)
        if h is None:
            continue
        r.upper_bound = h.get("upper_bound")
        r.lower_bound = h.get("lower_bound")
        r.gap = h.get("gap")
        r.converged = bool(h.get("converged"))
        r.cuts_added = h.get("cuts_added")
    return s


# ─── Log-parsing primary source ──────────────────────────────────────────────
#
# Format (one cascade level shown):
#
#   [TS] [info] Cascade [<level>]: new solver (max_iters=N, apertures=K, tol=T)
#   [TS] [info] ─── iter I (async) ───  elapsed= X.Xs  ETA= Y.Ym  [CONVERGED]
#   [TS] [info]     iter I  obj    UB=...G  LB=...G  gap=±%  Δgap=±%  kappa=...
#   ...
#   [TS] [info] Cascade [<level>]: N iters, LB=..., UB=..., gap=..., new_cuts=K,
#                                 T.Ts (converged|stalled|max_iters)
#   [TS] [info] [DIAG] Cascade [<level>]: transition decision: ...
#
# The iter counter is GLOBAL (continues across levels — warmup 0-12,
# uninodal 13-..., transport ..., full_network ...).  Level attribution
# comes from the most recent ``Cascade [<level>]: new solver`` header.

LOG_TS_RE = re.compile(r"^\[(?P<ts>\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+)\]")
LOG_LEVEL_HEAD_RE = re.compile(
    r"Cascade \[(?P<level>[^\]]+)\]: new solver "
    r"\(max_iters=(?P<mi>\d+),\s*apertures=(?P<ap>[^,)]+)"
    r"(?:,\s*tol=(?P<tol>[\d.eE+\-]+))?\)"
)
LOG_ITER_HEAD_RE = re.compile(
    r"─── iter\s+(?P<i>\d+)\s*\([^)]*\)\s*───\s+elapsed=\s*"
    r"(?P<elapsed>[\d.]+\s*[smhd]?)\s*"
    r"(?:ETA=\s*[\d.]+\s*[smhd]?\s*)?"
    r"(?P<conv>\[CONVERGED\])?"
)
LOG_ITER_VALS_RE = re.compile(
    r"iter\s+(?P<i>\d+)\s+obj\s+"
    r"UB=(?P<ub>[\d.eE+\-]+[KMG]?)\s+"
    r"LB=(?P<lb>[\d.eE+\-]+[KMG]?)\s+"
    r"gap=\s*(?P<gap>[+\-]?[\d.]+)%\s+"
    r"Δgap=\s*(?P<dgap>[+\-]?[\d.]+)%\s+"
    r"kappa=(?P<k>[\d.eE+\-]+)"
)
LOG_LEVEL_SUMMARY_RE = re.compile(
    r"Cascade \[(?P<level>[^\]]+)\]:\s+(?P<n>\d+)\s+iters.*?"
    r"new_cuts=(?P<cuts>\d+).*?,\s*(?P<wall>[\d.]+)s\s+"
    r"\((?P<status>[^)]+)\)"
)

_SUFFIX = {"K": 1e3, "M": 1e6, "G": 1e9, "T": 1e12}


def _parse_money_token(tok: str) -> float | None:
    """Parse a number with optional KMG suffix as emitted by ``fmt_money``."""
    tok = tok.strip()
    if not tok:
        return None
    suffix = tok[-1].upper()
    if suffix in _SUFFIX:
        try:
            return float(tok[:-1]) * _SUFFIX[suffix]
        except ValueError:
            return None
    try:
        return float(tok)
    except ValueError:
        return None


def _parse_elapsed_token(tok: str) -> float:
    """Parse ``"27.4s"`` / ``"1.3m"`` / ``"2.1h"`` into seconds."""
    tok = tok.strip()
    if not tok:
        return 0.0
    suffix = tok[-1].lower() if tok[-1].isalpha() else ""
    body = tok[:-1] if suffix else tok
    try:
        v = float(body)
    except ValueError:
        return 0.0
    return {"s": v, "m": v * 60.0, "h": v * 3600.0, "d": v * 86400.0}.get(suffix, v)


def _parse_log_timestamp(line: str) -> float | None:
    """Extract a unix-epoch float from a log line's ``[YYYY-MM-DD HH:MM:SS.sss]``."""
    m = LOG_TS_RE.match(line)
    if not m:
        return None
    try:
        dt = datetime.strptime(m.group("ts"), "%Y-%m-%d %H:%M:%S.%f")
    except ValueError:
        return None
    return dt.timestamp()


def parse_log(log_file: Path) -> tuple[list[IterRow], list[dict]]:
    """Parse a gtopt log file for the full per-iter history.

    Returns ``(rows, level_metas)`` where:
      * ``rows`` is one ``IterRow`` per ``iter I obj UB=... LB=... ...`` line,
        attributed to its enclosing cascade level via the most recent
        ``Cascade [<level>]: new solver ...`` header.  When the run is plain
        SDDP (no cascade), the synthetic level name is ``"sddp"``.
      * ``level_metas`` is one dict per ``Cascade [<level>]: ...`` block with
        keys ``name``, ``max_iters``, ``apertures``, ``tol``, plus optional
        ``iters``, ``new_cuts``, ``wall_s``, ``status`` filled from the
        level-summary line when present.

    Safe to call on a partially-written log: incomplete blocks at the
    tail simply yield rows without a summary entry.  Empty / missing log
    file returns ``([], [])``.
    """
    rows: list[IterRow] = []
    metas: list[dict] = []
    if not log_file.is_file():
        return rows, metas

    current_level = "sddp"
    last_iter_meta: dict[int, dict] = {}  # iter -> {wall, ts, conv}
    try:
        text = log_file.read_text(errors="replace")
    except OSError:
        return rows, metas

    for line in text.splitlines():
        m = LOG_LEVEL_HEAD_RE.search(line)
        if m:
            current_level = m.group("level")
            tol = m.group("tol")
            metas.append(
                {
                    "name": current_level,
                    "max_iters": int(m.group("mi")),
                    "apertures": m.group("ap"),
                    "tol": float(tol) if tol else None,
                }
            )
            continue
        m = LOG_LEVEL_SUMMARY_RE.search(line)
        if m:
            name = m.group("level")
            # Attach to the most recent matching meta (level names can
            # repeat across phantom retries; pick the last one).
            for meta in reversed(metas):
                if meta["name"] == name and "iters" not in meta:
                    meta["iters"] = int(m.group("n"))
                    meta["new_cuts"] = int(m.group("cuts"))
                    meta["wall_s"] = float(m.group("wall"))
                    meta["status"] = m.group("status").strip()
                    break
            continue
        m = LOG_ITER_HEAD_RE.search(line)
        if m:
            i = int(m.group("i"))
            ts = _parse_log_timestamp(line)
            last_iter_meta[i] = {
                "elapsed_s": _parse_elapsed_token(m.group("elapsed")),
                "ts": ts,
                "converged": bool(m.group("conv")),
            }
            continue
        m = LOG_ITER_VALS_RE.search(line)
        if m:
            i = int(m.group("i"))
            ub = _parse_money_token(m.group("ub"))
            lb = _parse_money_token(m.group("lb"))
            try:
                gap = float(m.group("gap")) / 100.0
            except ValueError:
                gap = None
            # gtopt's actual windowed Δgap — store as a fraction (the
            # log emits a percent value).  This is the authoritative
            # convergence metric; render_table prefers it over the
            # per-iter |ΔUB|/|UB| proxy.
            try:
                delta_gap = float(m.group("dgap")) / 100.0
            except (ValueError, IndexError):
                delta_gap = None
            try:
                kappa = float(m.group("k"))
            except ValueError:
                kappa = None
            meta = last_iter_meta.get(i, {})
            ts = meta.get("ts") or _parse_log_timestamp(line)
            rows.append(
                IterRow(
                    level=current_level,
                    iter=i,
                    mtime=ts if ts is not None else 0.0,
                    upper_bound=ub,
                    lower_bound=lb,
                    gap=gap,
                    delta_gap=delta_gap,
                    converged=meta.get("converged", False),
                    kappa=kappa,
                )
            )
            continue
    return rows, metas


def attach_kappa_from_log(rows: list[IterRow], logs_dir: Path) -> None:
    """Best-effort: parse the gtopt log file for per-iter kappa.

    Looks for lines matching the standard ``iter K`` headline with a
    ``kappa=N.NNe+NN`` clause.  Silent no-op if logs aren't found.
    """
    if not logs_dir.is_dir():
        return
    log_files = sorted(
        (p for p in logs_dir.iterdir() if LOG_FILE_RE.match(p.name)),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    if not log_files:
        return
    # Pattern matches a per-iter line that mentions kappa.
    kappa_re = re.compile(r"\biter\s+(?P<i>\d+)\b.*?\bkappa=(?P<k>[0-9.eE+\-]+)")
    by_iter: dict[int, float] = {}
    for log in log_files:
        try:
            text = log.read_text(errors="replace")
        except OSError:
            continue
        for m in kappa_re.finditer(text):
            try:
                by_iter[int(m.group("i"))] = float(m.group("k"))
            except ValueError:
                continue
    for r in rows:
        if r.iter in by_iter:
            r.kappa = by_iter[r.iter]


# ─── Table rendering ─────────────────────────────────────────────────────────


def split_by_level(rows: list[IterRow], cfgs: list[LevelConfig]) -> list[LevelStats]:
    """Group rows by their level dirname; preserve cfg order, append unknowns."""
    by_name: dict[str, LevelStats] = {c.name: LevelStats(cfg=c) for c in cfgs}
    for r in rows:
        if r.level not in by_name:
            by_name[r.level] = LevelStats(cfg=LevelConfig(name=r.level))
        by_name[r.level].rows.append(r)
    # Sort each level's rows by iter then mtime
    for lvl in by_name.values():
        lvl.rows.sort(key=lambda r: (r.iter, r.mtime))
    # Order: configured cfgs first (in input JSON order), then any extras.
    ordered: list[LevelStats] = []
    seen = set()
    for c in cfgs:
        if c.name in by_name and by_name[c.name].rows:
            ordered.append(by_name[c.name])
            seen.add(c.name)
    for name, lvl in by_name.items():
        if name not in seen and lvl.rows:
            ordered.append(lvl)
    return ordered


def render_table(
    stats: list[LevelStats],
    *,
    case_name: str,
    solver_status: dict,
    width: int = 119,
) -> str:
    """Format the multi-level convergence table as a single string."""
    out: list[str] = []
    out.append(f"● Cascade SDDP convergence table — {case_name}")
    out.append("")

    # Header line with solve-start / now / elapsed.
    if stats and stats[0].rows:
        start_ts = stats[0].rows[0].mtime
        now_ts = max(r.mtime for s in stats for r in s.rows)
        elapsed = now_ts - start_ts
        start_dt = datetime.fromtimestamp(start_ts)
        now_dt = datetime.fromtimestamp(now_ts)
        out.append(
            f"  Solve start: {start_dt:%H:%M:%S}  |  "
            f"Currently: {now_dt:%H:%M:%S}  |  Elapsed: {fmt_wall(elapsed)}"
        )
        out.append("")

    bar = "═" * width
    sep = "─" * width
    iter_header = (
        f"  {'iter':>4}   {'UB':>7}    {'LB':>7}    "
        f"{'gap':>11}   {'Δgap':>11}   {'kappa':>9}    "
        f"{'iter_wall':>10}   {'level_wall':>11}   {'total_wall':>11}    note"
    )

    # Establish absolute zero for `total_wall` = mtime of the first iter we saw.
    if not stats or not stats[0].rows:
        out.append("  (no cut files yet — run hasn't produced an iteration)")
        return "\n".join(out)
    t0 = stats[0].rows[0].mtime

    prev_level_end_mtime: float | None = None
    cum_serialised = 0
    # Cross-level UB anchor for the cascade-Δgap override.  Carries
    # the LAST iter's UB of the previous level into the FIRST iter
    # of the new level so the first-iter Δgap reflects the
    # cross-level UB change (the actual policy delta induced by
    # switching apertures / model fidelity) rather than gtopt's
    # per-level "no prior" 100 % sentinel.  This is DISPLAY-only —
    # the C++ solver intentionally resets its own Δgap history at
    # every level so stationary convergence cannot fire on iter 1
    # of a new level just because the inherited cuts happen to
    # match the previous level's UB.  See option A discussion in
    # the 2026-05-15 conversation for why we do not seed the
    # solver itself.
    cross_level_prev_ub: float | None = None
    for lvl in stats:
        c = lvl.cfg
        out.append(f"  {bar}")
        header_fields = []
        if c.max_iterations is not None:
            header_fields.append(f"max_iters={c.max_iterations}")
        if c.num_apertures is not None:
            header_fields.append(f"apertures={c.num_apertures}")
        if c.convergence_tol is not None:
            header_fields.append(f"tol={c.convergence_tol:g}")
        if c.stationary_tol is not None:
            header_fields.append(f"stationary_tol={c.stationary_tol:g}")
        if c.stationary_gap_ceiling is not None:
            header_fields.append(f"gap_ceiling={c.stationary_gap_ceiling:g}")
        out.append(f"  LEVEL: {c.name:<14} " + "  ".join(header_fields))
        out.append(f"  {sep}")
        out.append(iter_header)
        out.append(f"  {sep}")
        if prev_level_end_mtime is not None and lvl.rows:
            transition_s = lvl.rows[0].mtime - prev_level_end_mtime
            out.append(
                f"  inherited: {cum_serialised:,} cuts; "
                f"transition ≈ {transition_s:.0f}s"
            )

        level_start = lvl.rows[0].mtime
        prev_ub: float | None = None
        prev_mtime: float | None = None
        new_cuts = 0
        for row_idx, r in enumerate(lvl.rows):
            if prev_mtime is None:
                iter_wall_s = r.mtime - level_start
            else:
                iter_wall_s = r.mtime - prev_mtime
            prev_mtime = r.mtime
            level_wall = r.mtime - level_start
            total_wall = r.mtime - t0

            # Δgap: prefer gtopt's WINDOWED Δgap from the log
            # (``r.delta_gap``, populated by ``parse_log``).  This is
            # the authoritative metric the SDDP convergence check
            # evaluates against ``stationary_tol`` — it uses a
            # multi-iter lookback to filter sample noise, so it can
            # differ substantially from a naïve per-iter |ΔUB|/|UB|.
            # Pre-fix, the table showed the proxy and misled the user
            # into thinking convergence should fire one iter earlier
            # than it actually does (e.g. iter 18 proxy = 0.24 % vs
            # gtopt's actual Δgap = 1.20 % at the same iter).
            #
            # Cross-level override: gtopt emits Δgap = 100 % (the
            # "no prior" sentinel) at the FIRST iter of every cascade
            # level because its per-level iter tracker resets on
            # construction.  For the display table that sentinel is
            # uninformative — every cascade boundary shows a misleading
            # +100 % jump.  When this row is the first of a non-first
            # level AND we have a carried `cross_level_prev_ub` from
            # the previous level's tail, recompute Δgap against the
            # cross-level anchor instead.  The solver's own
            # convergence check still uses its per-level reset value;
            # this override is purely for the table.
            delta_gap: float | None = r.delta_gap
            is_first_row_of_non_first_level = (
                row_idx == 0 and cross_level_prev_ub is not None
            )
            if (
                is_first_row_of_non_first_level
                and r.upper_bound is not None
                and cross_level_prev_ub != 0
            ):
                delta_gap = abs(r.upper_bound - cross_level_prev_ub) / abs(
                    cross_level_prev_ub
                )
            elif delta_gap is None:
                if r.upper_bound is not None and prev_ub is not None and prev_ub != 0:
                    delta_gap = abs(r.upper_bound - prev_ub) / abs(prev_ub)
                elif r.upper_bound is not None and prev_ub is None:
                    # First iter we see with UB — no lookback to compute Δ.
                    delta_gap = 1.0  # sentinel matches gtopt's "no prior" default
            # Update lookback anchor only when we have a real UB.
            if r.upper_bound is not None:
                prev_ub = r.upper_bound

            new_cuts += r.cuts_added or 0
            note = "[CONVERGED]" if r.converged else ""

            out.append(
                f"  {r.iter:>4d}   {fmt_money(r.upper_bound):>7}    "
                f"{fmt_money(r.lower_bound):>7}    "
                f"{fmt_pct(r.gap):>11}   {fmt_pct(delta_gap):>11}   "
                f"{fmt_kappa(r.kappa):>9}    "
                f"{fmt_wall(iter_wall_s):>10}   "
                f"{fmt_wall(level_wall):>11}   "
                f"{fmt_wall(total_wall):>11}    {note}"
            )

        if lvl.rows:
            level_total = lvl.rows[-1].mtime - lvl.rows[0].mtime
            # Prefer the level-summary `new_cuts=` from the log when
            # available — it's authoritative and includes the post-
            # iter-12 simulation pass that the per-row `cuts_added`
            # accumulator (computed from `r.cuts_added`, which the log
            # path doesn't populate) cannot reach.
            level_new_cuts = lvl.new_cuts_total if lvl.new_cuts_total > 0 else new_cuts
            cum_serialised += level_new_cuts
            out.append(
                f"  ─── {len(lvl.rows)} iters, level total "
                f"{level_total:.1f}s, new_cuts={level_new_cuts:,} → "
                f"{cum_serialised:,} serialised"
            )
            prev_level_end_mtime = lvl.rows[-1].mtime
            # Carry the last iter's UB into the NEXT level so its
            # first-iter Δgap can be computed as a cross-level
            # |ΔUB|/|UB| (overriding gtopt's per-level 100 %
            # sentinel for display).  Skip when the level emitted
            # no real UB (only sentinel rows).
            if lvl.rows[-1].upper_bound is not None:
                cross_level_prev_ub = lvl.rows[-1].upper_bound
        out.append("")

    # Final status line.
    if solver_status:
        out.append(
            f"  ─ status: {solver_status.get('status', '?')}  "
            f"iter={solver_status.get('iteration', '?')}  "
            f"converged={solver_status.get('converged', False)}  "
            f"UB={fmt_money(solver_status.get('upper_bound'))}  "
            f"LB={fmt_money(solver_status.get('lower_bound'))}  "
            f"gap={fmt_pct(solver_status.get('gap'))}  "
            f"elapsed={fmt_wall(solver_status.get('elapsed_s', 0))}"
        )

    return "\n".join(out)


# ─── Main ────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Render a gtopt cascade SDDP convergence table from a "
        "case run directory.",
    )
    parser.add_argument(
        "case_dir",
        type=Path,
        help="Run directory containing the input JSON and `results/`.",
    )
    parser.add_argument(
        "--json",
        dest="input_json",
        type=Path,
        default=None,
        help="Override the input JSON path (default: auto-detect).",
    )
    parser.add_argument(
        "--results",
        dest="results_dir",
        type=Path,
        default=None,
        help="Override the results directory (default: <case_dir>/results).",
    )
    parser.add_argument(
        "--width", type=int, default=119, help="Table width (default: 119)."
    )
    parser.add_argument(
        "--all-levels",
        action="store_true",
        help=(
            "Bypass the `min_mtime` stale-file filter so every cut file in "
            "every `cuts/<level>/` directory contributes to the table.  "
            "Required when rendering a multi-level cascade where earlier "
            "levels' cuts predate the live `solver_status.json` timestamp "
            "(the per-level run start) and would otherwise be filtered out "
            "as 'stale'.  Safe whenever the `cuts/` directory was cleared "
            "before the cascade started (no actual stale files present)."
        ),
    )
    args = parser.parse_args(argv)

    case_dir: Path = args.case_dir.resolve()
    if not case_dir.is_dir():
        sys.exit(f"case_dir not found: {case_dir}")

    input_json = args.input_json or find_input_json(case_dir)
    results_dir = args.results_dir or (case_dir / "results")
    cuts_dir = results_dir / "cuts"
    solver_status = results_dir / "solver_status.json"
    # gtopt writes its log under `<case_dir>/output/logs/gtopt_N.log` by
    # default (the `output` subdir is created by `setup_file_logging`,
    # independent of the `results/` directory used for cut/parquet
    # output).  Search several conventional locations so the script
    # works regardless of where the run was launched from.
    log_search_dirs = [
        case_dir / "output" / "logs",
        results_dir / "logs",
        case_dir / "logs",
    ]
    logs_dir = next(
        (d for d in log_search_dirs if d.is_dir()),
        log_search_dirs[0],
    )

    cfgs = parse_level_configs(input_json)

    # Derive the current run's start time from solver_status.json's
    # `timestamp - elapsed_s` to filter out stale cut files from
    # prior runs in the same `cuts/` directory.  The SDDP solver
    # only overwrites cut files for iter numbers it actually reaches
    # in the current run; iters > current_iter persist as stale
    # leftovers and would corrupt the per-iter wall-time deltas.
    min_mtime: float | None = None
    if not args.all_levels and solver_status.is_file():
        try:
            with solver_status.open() as f:
                s_peek = json.load(f)
            ts = s_peek.get("timestamp")
            elapsed = s_peek.get("elapsed_s")
            if isinstance(ts, (int, float)) and isinstance(elapsed, (int, float)):
                # `timestamp` is the last status-write unix time; subtract
                # elapsed to get the run's start time.  Subtract a small
                # slack (5s) so the very first cut file (which may
                # predate the status snapshot by a moment) isn't filtered.
                min_mtime = float(ts) - float(elapsed) - 5.0
        except (OSError, json.JSONDecodeError, KeyError, ValueError):
            pass

    # Primary source: parse the gtopt log file directly.  Survives across
    # cascade level transitions (solver_status.json is a rolling
    # snapshot; once L1 starts, L0's per-iter UB/LB are gone).  Falls
    # back to cuts + solver_status only when no log is reachable.
    log_file: Path | None = None
    if logs_dir.is_dir():
        log_candidates = sorted(
            (p for p in logs_dir.iterdir() if LOG_FILE_RE.match(p.name)),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if log_candidates:
            log_file = log_candidates[0]

    log_rows: list[IterRow] = []
    log_metas: list[dict] = []
    if log_file is not None:
        log_rows, log_metas = parse_log(log_file)

    if log_rows:
        # Log-based path: use log_rows directly.  Cut-file mtimes are
        # only used if the log entry lacks a timestamp (shouldn't happen
        # for well-formed gtopt logs, but defensive).
        rows = log_rows
        # Fill mtimes from cut files for rows missing a timestamp.
        if any(r.mtime == 0.0 for r in rows):
            cut_rows = collect_iter_rows(cuts_dir, min_mtime=None)
            cut_mt = {(r.level, r.iter): r.mtime for r in cut_rows}
            for r in rows:
                if r.mtime == 0.0:
                    r.mtime = cut_mt.get((r.level, r.iter), 0.0)
        # Merge log-derived level metas into the JSON-derived cfgs:
        # adopt the actually-used max_iters / apertures / tol from the
        # log when present (a CLI `--set` override at run time can
        # differ from the JSON).
        cfgs_by_name = {c.name: c for c in cfgs}
        for meta in log_metas:
            name = meta["name"]
            cfg = cfgs_by_name.get(name)
            if cfg is None:
                cfg = LevelConfig(name=name)
                cfgs_by_name[name] = cfg
                cfgs.append(cfg)
            if meta.get("max_iters") is not None:
                cfg.max_iterations = meta["max_iters"]
            ap = meta.get("apertures")
            if ap not in (None, "", "all"):
                try:
                    cfg.num_apertures = int(ap)
                except ValueError:
                    pass
        # solver_status still useful for the live status footer.
        status = {}
        if solver_status.is_file():
            try:
                with solver_status.open() as f:
                    status = json.load(f)
            except (OSError, json.JSONDecodeError):
                status = {}
    else:
        # Fallback: cuts + solver_status (legacy path).
        rows = collect_iter_rows(cuts_dir, min_mtime=min_mtime)
        status = merge_history(rows, solver_status)
        attach_kappa_from_log(rows, logs_dir)

    stats = split_by_level(rows, cfgs)
    # Carry per-level new_cuts from the log summary into stats so the
    # ──── footer prints the actually-serialised counts (the
    # cut-file-only path produces 0 here once `min_mtime` filters older
    # levels — log-based path is authoritative).
    if log_metas:
        meta_by_name = {m["name"]: m for m in log_metas}
        for s in stats:
            meta = meta_by_name.get(s.cfg.name)
            if meta and "new_cuts" in meta:
                s.new_cuts_total = meta["new_cuts"]
    print(
        render_table(
            stats, case_name=case_dir.name, solver_status=status, width=args.width
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
