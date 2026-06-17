#!/usr/bin/env python3
"""gtopt_convergence_table — pretty-print a cascade SDDP convergence table.

Primary source: ``results/logs/gtopt*.log`` (the gtopt run log).  Each SDDP
iteration writes two lines:

    ─── iter N (async) ───  elapsed= XXs  [ETA= YYm] [CONVERGED]
        iter N  obj    ub=...G  lb=...G  gap=±NN%  Δgap=±NN%  kappa=...

And each cascade level is bracketed by:

    Cascade [<name>]: new solver (max_iters=..., apertures=..., tol=...)
    ...
    Cascade [<name>]: N iters, lb=..., ub=..., gap=..., new_cuts=K, T.Ts (converged)
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
    """Format a money-like number as e.g. ``2.87451G`` / ``18.4567M`` /
    ``42.123k``.  Precision: 5dp for G / 4dp for M / 3dp for k / 2dp for
    bare units — enough to distinguish multi-million-dollar SDDP bounds
    that differ in the 4th–5th significant digit (e.g. UB drift across
    iter-by-iter Δgap convergence)."""
    if x is None:
        return "—"
    a = abs(x)
    if a >= 1e9:
        return f"{x / 1e9:.5f}G"
    if a >= 1e6:
        return f"{x / 1e6:.4f}M"
    if a >= 1e3:
        return f"{x / 1e3:.3f}k"
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
    # Seconds since the solve started, parsed from the log's
    # ``iter N (async) … elapsed= Xs`` clause.  Used to give the FIRST
    # iteration of the first level a real wall time (it has no previous
    # cut/level boundary to subtract from).  ``None`` when no log line
    # carried an elapsed token (e.g. cut-file-only fallback).
    elapsed_s: float | None = None


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
#   [TS] [info]     iter I  obj    ub=...G  lb=...G  gap=±%  Δgap=±%  kappa=...
#   ...
#   [TS] [info] Cascade [<level>]: N iters, lb=..., ub=..., gap=..., new_cuts=K,
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
    # Accept BOTH casings of each letter:
    # ``ub`` (current, lowercase = raw in-phase bound) and ``UB`` (legacy
    # / alpha-corrected per the LB/UB casing convention).  Same for
    # ``lb``/``LB`` and ``gap``/``Gap``.  Previously this regex hard-
    # coded the second letter to upper-case, so the gtopt log lines
    # emitted with lower-case both letters (``ub=…`` / ``lb=…``) failed
    # the match and the table fell back to "—" for UB/LB.
    r"[Uu][Bb]=(?P<ub>[\d.eE+\-]+[KMG]?)\s+"
    r"[Ll][Bb]=(?P<lb>[\d.eE+\-]+[KMG]?)\s+"
    r"[Gg]ap=\s*(?P<gap>[+\-]?[\d.]+)%\s+"
    r"Δ[Gg]ap=\s*(?P<dgap>[+\-]?[\d.]+)%\s+"
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


def log_first_timestamp(log_file: Path) -> float | None:
    """Unix-epoch of the FIRST ``[YYYY-MM-DD HH:MM:SS.sss]`` line in the log.

    This is the gtopt process start — emitted before the bootstrap
    iteration's cut is serialised — so anchoring the table's solve_start
    here gives the first iteration a real wall time (LP build + first
    solve) instead of 0.  ``None`` when the file is missing/unreadable or
    has no timestamped line.
    """
    try:
        with log_file.open(encoding="utf-8", errors="replace") as fh:
            for line in fh:
                ts = _parse_log_timestamp(line)
                if ts is not None:
                    return ts
    except OSError:
        return None
    return None


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
                    elapsed_s=meta.get("elapsed_s"),
                )
            )
            continue

    # Drain rows were previously synthesized here from
    # ``SDDP Async [iN sS]: completed …`` log lines so the per-level
    # iteration counter visually flowed (iter N-1 [CONVERGED] → iter N
    # [drain — discarded] → next level).  Removed 2026-05-17: the
    # cascade controller does NOT count those async completions as
    # iterations — they are leftover worker tasks that began before
    # the master declared convergence and whose bound contributions
    # are discarded (cf. ``source/cascade_method.cpp:639`` —
    # ``skip_simulation_pass=true`` for every non-terminal level,
    # i.e. no sim pass between levels).  Showing them as table rows
    # made readers reach for "L0 final simulation slot" /
    # "verification pass" explanations that conflate iter-counter
    # bookkeeping with real work.  See feedback memory
    # ``feedback_no_inter_level_sim``.
    #
    # The iteration-counter gap between converged level N-1 and
    # first iter of level N (e.g. uninodal iter 23 → transport iter
    # 25, skipping 24) reflects ``global_iter_index = next(...)``
    # advancement in the cascade controller (``cascade_method.cpp:
    # 968``) — the level-boundary number is reserved to keep iter
    # labels globally monotonic in the cut store but represents no
    # discoverable work.

    # Per-level rows by iter then mtime (mirrors ``split_by_level``
    # so the renderer sees them ordered).
    rows.sort(key=lambda r: (r.level, r.iter, r.mtime))
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
    solve_start_override: float | None = None,
    width: int = 119,
) -> str:
    """Format the multi-level convergence table as a single string."""
    out: list[str] = []
    out.append(f"● Cascade SDDP convergence table — {case_name}")
    out.append("")

    # Header line with solve-start / now / elapsed.
    if stats and stats[0].rows:
        # True solve start: the log's first line timestamp (process start,
        # captured before the bootstrap iteration) when available — this is
        # what makes the FIRST iteration show its real wall (LP build + first
        # solve) instead of 0.  Fall back to the first iter's timestamp minus
        # its logged elapsed (the bootstrap row often has no elapsed token).
        start_ts = (
            solve_start_override
            if solve_start_override is not None
            else stats[0].rows[0].mtime - (stats[0].rows[0].elapsed_s or 0.0)
        )
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
    # Two Δgap columns:
    #   Δgap(gt)   = gtopt's WINDOWED stationary metric parsed from
    #                the log (the value the solver compares against
    #                `stationary_tol`).
    #   Δgap(now)  = tool-computed per-iter |ΔUB|/|UB|, no windowing.
    #                Useful for spotting iters where the policy moved
    #                a lot at this single step even if the windowed
    #                metric stays small (or vice versa).
    iter_header = (
        f"  {'iter':>4}   {'UB':>7}    {'LB':>7}    "
        f"{'gap':>11}   {'Δgap(gt)':>11}   {'Δgap(now)':>11}   "
        f"{'kappa':>9}    "
        f"{'iter_wall':>10}   {'level_wall':>11}   {'total_wall':>11}    note"
    )

    # Establish absolute zero for `total_wall`.  The first iteration's
    # cut/log timestamp is when it *finished*, not when the solve began,
    # so anchor at the true solve start = first-iter timestamp minus its
    # logged elapsed.  This makes the FIRST iteration of the first level
    # show its real wall time instead of 0 (falls back to the first
    # timestamp when no elapsed token is available).
    if not stats or not stats[0].rows:
        out.append("  (no cut files yet — run hasn't produced an iteration)")
        return "\n".join(out)
    first_row = stats[0].rows[0]
    solve_start = (
        solve_start_override
        if solve_start_override is not None
        else first_row.mtime - (first_row.elapsed_s or 0.0)
    )
    t0 = solve_start

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
    # Display-only contiguous iteration numbering across cascade levels.
    # The C++ solver tags each cut at iteration `display+1`, and at every
    # level boundary `load_cuts` advances the offset to
    # `next(max_cut_iteration)` — one past the inherited cuts — so the
    # raw async indices skip a number per boundary (warmup 0-3, uninodal
    # 5-9, transport 11-16, …).  That gap is a cut-store accounting
    # artifact, not a missed iteration: the bounds are correct.  Renumber
    # contiguously here for presentation only; `r.iter` (used for the
    # kappa lookup and log matching) is left untouched.
    display_iter = stats[0].rows[0].iter
    for lvl in stats:
        c = lvl.cfg
        out.append(f"  {bar}")
        header_fields = []
        if c.max_iterations is not None:
            header_fields.append(f"max_iters={c.max_iterations}")
        if c.num_apertures is not None:
            header_fields.append(f"apertures={c.num_apertures}")
        # `convergence_tol` (the legacy |gap| threshold) is no longer
        # consumed by gtopt — the post-2026-05-14 SDDP convergence path
        # is purely ΔUB stationarity (see `sddp_iteration.cpp`).  Hide
        # it from the level header so the displayed knobs match what
        # the solver actually checks.
        if c.stationary_tol is not None:
            header_fields.append(f"stationary_tol={c.stationary_tol:g}")
        # ``stationary_gap_ceiling`` is intentionally NOT shown: the
        # |gap| < ceiling convergence gate was removed (2026-06-16) — ΔUB
        # stationarity is the sole signal — so the field is vestigial and
        # displaying it misrepresents what the solver checks.
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

        # Anchor the level's wall clock at its boundary, not at its first
        # iteration's timestamp — otherwise the first iteration's iter_wall
        # is `mtime - mtime = 0`.  For a non-first level the boundary is the
        # previous level's last iteration (so the first iter_wall captures
        # the inter-level transition + cold first iteration).  For the first
        # level it is the true solve start.
        level_start = (
            prev_level_end_mtime if prev_level_end_mtime is not None else solve_start
        )
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

            # Two Δgap values shown side by side:
            #
            #   delta_gap_gt — gtopt's WINDOWED Δgap from the log
            #     (``r.delta_gap``, populated by ``parse_log``).  This
            #     is the authoritative metric the SDDP convergence
            #     check evaluates against ``stationary_tol``.  Uses a
            #     multi-iter lookback to filter sample noise, so it
            #     can differ substantially from a naïve per-iter
            #     |ΔUB|/|UB|.
            #
            #   delta_gap_now — tool-computed per-iter |ΔUB|/|UB|
            #     against the PREVIOUS row's UB (or the cross-level
            #     anchor for the first row of a non-first level).
            #     No windowing.  Useful for spotting iters where the
            #     policy moved a lot at this single step even if
            #     gtopt's windowed metric stays small (or vice
            #     versa) — and for cross-checking the parser.
            #
            # Cross-level override (applies to both): gtopt emits
            # Δgap = 100 % (the "no prior" sentinel) at the FIRST iter
            # of every cascade level because its per-level iter
            # tracker resets.  For the table that sentinel is
            # uninformative — every cascade boundary shows a misleading
            # +100 % jump.  When this row is the first of a non-first
            # level AND we have a carried `cross_level_prev_ub` from
            # the previous level's tail, recompute against the
            # cross-level anchor for BOTH columns.
            delta_gap_gt: float | None = r.delta_gap
            delta_gap_now: float | None = None

            is_first_row_of_non_first_level = (
                row_idx == 0 and cross_level_prev_ub is not None
            )
            if (
                is_first_row_of_non_first_level
                and r.upper_bound is not None
                and cross_level_prev_ub != 0
            ):
                cross = abs(r.upper_bound - cross_level_prev_ub) / abs(
                    cross_level_prev_ub
                )
                delta_gap_gt = cross
                delta_gap_now = cross
            else:
                # Tool-side per-iter Δgap against prev_ub.
                if r.upper_bound is not None and prev_ub is not None and prev_ub != 0:
                    delta_gap_now = abs(r.upper_bound - prev_ub) / abs(prev_ub)
                elif r.upper_bound is not None and prev_ub is None:
                    delta_gap_now = 1.0  # sentinel for "no prior"
                # Fill gtopt-side from tool-side only when the log
                # parse genuinely produced None (i.e. the log line
                # had no Δgap field) — never overwrite a real value.
                if delta_gap_gt is None:
                    delta_gap_gt = delta_gap_now
            # Update lookback anchor only when we have a real UB.
            if r.upper_bound is not None:
                prev_ub = r.upper_bound

            new_cuts += r.cuts_added or 0 if (r.cuts_added or 0) > 0 else 0
            note = "[CONVERGED]" if r.converged else ""

            out.append(
                f"  {display_iter:>4d}   {fmt_money(r.upper_bound):>7}    "
                f"{fmt_money(r.lower_bound):>7}    "
                f"{fmt_pct(r.gap):>11}   "
                f"{fmt_pct(delta_gap_gt):>11}   "
                f"{fmt_pct(delta_gap_now):>11}   "
                f"{fmt_kappa(r.kappa):>9}    "
                f"{fmt_wall(iter_wall_s):>10}   "
                f"{fmt_wall(level_wall):>11}   "
                f"{fmt_wall(total_wall):>11}    {note}"
            )
            display_iter += 1

        if lvl.rows:
            # Measure from the level boundary (matches level_wall above), so
            # the level total includes the inter-level transition + cold
            # first iteration rather than only the iteration-to-iteration span.
            level_total = lvl.rows[-1].mtime - level_start
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
        "--log",
        dest="log_path",
        type=Path,
        default=None,
        help=(
            "Explicit path to the gtopt log (a `gtopt_*.log` file, or a "
            "directory containing them — newest wins).  Use this when the "
            "run's `output_directory` / launch cwd differs from `case_dir` "
            "so the log lives outside the auto-probed locations.  Without a "
            "reachable log the table degrades to a cuts-only view (no "
            "per-iter UB/LB/gap) and prints a warning."
        ),
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
    # output).  When the run is launched from the case's parent
    # directory the log instead lands under `<parent>/output/logs/` —
    # so we also probe one level up.  Pick the FIRST candidate that
    # actually contains a `gtopt_*.log` file, not just the first that
    # exists: prior behaviour stopped at an empty `case_dir/output/
    # logs/` and never fell through to the populated alternative,
    # which dropped the tool back to the cut-file fallback path with
    # no UB / LB / kappa per iter.
    # gtopt writes `output/logs/` relative to its LAUNCH cwd, which is most
    # often the directory holding the input JSON — so probe there too
    # (handles `output_directory` pointed elsewhere than the launch dir).
    log_search_dirs = [
        case_dir / "output" / "logs",
        results_dir / "logs",
        results_dir.parent / "output" / "logs",
        case_dir / "logs",
        case_dir.parent / "output" / "logs",
        input_json.parent / "output" / "logs",
        input_json.parent / "logs",
    ]

    def _newest_log_mtime(d: Path) -> float:
        """Newest ``gtopt_*.log`` mtime in ``d``; -1.0 if none / not a dir."""
        if not d.is_dir():
            return -1.0
        try:
            return max(
                (p.stat().st_mtime for p in d.iterdir() if LOG_FILE_RE.match(p.name)),
                default=-1.0,
            )
        except OSError:
            return -1.0

    # Choose the log directory so a stale ``output/logs/`` from an earlier
    # (killed) run can't shadow the run we're actually rendering:
    #   1. An explicit ``--results`` points at one run's cuts; its sibling
    #      ``logs/`` is THAT run's log — use it whenever it has one.
    #   2. Otherwise pick the candidate whose NEWEST log is the most recent,
    #      not merely the first candidate that happens to contain any log.
    if args.results_dir is not None and _newest_log_mtime(results_dir / "logs") >= 0.0:
        logs_dir = results_dir / "logs"
    else:
        logs_dir = max(log_search_dirs, key=_newest_log_mtime)
        if _newest_log_mtime(logs_dir) < 0.0:
            logs_dir = next(
                (d for d in log_search_dirs if d.is_dir()), log_search_dirs[0]
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
    def _newest_log_in(d: Path) -> Path | None:
        """Newest ``gtopt_*.log`` in directory ``d`` (None if none)."""
        if not d.is_dir():
            return None
        cands = sorted(
            (p for p in d.iterdir() if LOG_FILE_RE.match(p.name)),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        return cands[0] if cands else None

    log_file: Path | None = None
    if args.log_path is not None:
        # Explicit override: accept either a log file or a directory of logs.
        if args.log_path.is_dir():
            log_file = _newest_log_in(args.log_path)
            if log_file is None:
                print(
                    f"WARNING: --log dir {args.log_path} has no gtopt_*.log",
                    file=sys.stderr,
                )
        elif args.log_path.is_file():
            log_file = args.log_path
        else:
            print(
                f"WARNING: --log path not found: {args.log_path}",
                file=sys.stderr,
            )
    if log_file is None:
        log_file = _newest_log_in(logs_dir)

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
        # Fallback: cuts + solver_status (legacy path).  No log was
        # reachable, so per-iter UB/LB/gap/kappa cannot be recovered for
        # past levels — every row but the live one renders as "—".  Warn
        # LOUDLY so a degraded table is never mistaken for a broken solve.
        searched = "\n".join(f"    {d}" for d in log_search_dirs)
        print(
            "WARNING: no gtopt_*.log found — convergence table degraded to "
            "the cuts-only view\n"
            "         (per-iter UB/LB/gap/kappa unavailable; only the live "
            "iter has bounds).\n"
            "         Point --log at the run's log file/dir to get the full "
            "table.  Searched:\n"
            f"{searched}",
            file=sys.stderr,
        )
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
    # Anchor the table's solve_start at the log's first timestamp (the
    # process start) so the first iteration's wall reflects the LP build +
    # first solve rather than 0.
    solve_start_override = (
        log_first_timestamp(log_file) if log_file is not None else None
    )
    print(
        render_table(
            stats,
            case_name=case_dir.name,
            solver_status=status,
            solve_start_override=solve_start_override,
            width=args.width,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
