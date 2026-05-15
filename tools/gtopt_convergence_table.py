#!/usr/bin/env python3
"""gtopt_convergence_table — pretty-print a cascade SDDP convergence table.

Reads three sources from a gtopt run directory:

  1. The input JSON (cascade level configs: name, max_iterations,
     num_apertures, convergence_tol, stationary_tol, stationary_gap_ceiling).
  2. ``results/solver_status.json`` (recent-iter UB/LB/gap/converged).
  3. ``results/cuts/<level>/sddp_cuts_*.parquet`` mtimes (per-iter wall time
     anchors; survive in-flight runs since cut files are written at the end
     of each iteration's save_cuts pass).

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
    """Format a fraction as ``+12.34%`` (signed, 2dp)."""
    if x is None:
        return "—"
    return f"{x:+.2%}"


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


def collect_iter_rows(cuts_dir: Path) -> list[IterRow]:
    """Walk ``cuts/<level>/sddp_cuts_*.parquet`` to anchor per-iter mtimes."""
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
            rows.append(
                IterRow(
                    level=level_dir.name,
                    iter=int(m.group(1)),
                    mtime=f.stat().st_mtime,
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
        f"{'gap':>9}   {'Δgap':>9}   {'kappa':>9}    "
        f"{'iter_wall':>10}   {'level_wall':>11}   {'total_wall':>11}    note"
    )

    # Establish absolute zero for `total_wall` = mtime of the first iter we saw.
    if not stats or not stats[0].rows:
        out.append("  (no cut files yet — run hasn't produced an iteration)")
        return "\n".join(out)
    t0 = stats[0].rows[0].mtime

    prev_level_end_mtime: float | None = None
    cum_serialised = 0
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
        for r in lvl.rows:
            if prev_mtime is None:
                iter_wall_s = r.mtime - level_start
            else:
                iter_wall_s = r.mtime - prev_mtime
            prev_mtime = r.mtime
            level_wall = r.mtime - level_start
            total_wall = r.mtime - t0

            # Δgap proxy: relative ΔUB from previous iter in this level.
            # We can only compute it when BOTH the current and prior UB
            # are present.  If history was pruned for either iter, leave
            # Δgap blank — better than showing the "no-lookback" sentinel
            # that misleadingly suggests a 100 % delta.
            delta_ub: float | None = None
            if r.upper_bound is not None and prev_ub is not None and prev_ub != 0:
                delta_ub = abs(r.upper_bound - prev_ub) / abs(prev_ub)
            elif r.upper_bound is not None and prev_ub is None:
                # First iter we see with UB — no lookback to compute Δ.
                delta_ub = 1.0  # sentinel matches gtopt's "no prior" default
            # Update lookback anchor only when we have a real UB.
            if r.upper_bound is not None:
                prev_ub = r.upper_bound

            new_cuts += r.cuts_added or 0
            note = "[CONVERGED]" if r.converged else ""

            out.append(
                f"  {r.iter:>4d}   {fmt_money(r.upper_bound):>7}    "
                f"{fmt_money(r.lower_bound):>7}    "
                f"{fmt_pct(r.gap):>9}   {fmt_pct(delta_ub):>9}   "
                f"{fmt_kappa(r.kappa):>9}    "
                f"{fmt_wall(iter_wall_s):>10}   "
                f"{fmt_wall(level_wall):>11}   "
                f"{fmt_wall(total_wall):>11}    {note}"
            )

        if lvl.rows:
            level_total = lvl.rows[-1].mtime - lvl.rows[0].mtime
            cum_serialised += new_cuts
            out.append(
                f"  ─── {len(lvl.rows)} iters, level total "
                f"{level_total:.1f}s, new_cuts={new_cuts:,} → "
                f"{cum_serialised:,} serialised"
            )
            prev_level_end_mtime = lvl.rows[-1].mtime
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
    args = parser.parse_args(argv)

    case_dir: Path = args.case_dir.resolve()
    if not case_dir.is_dir():
        sys.exit(f"case_dir not found: {case_dir}")

    input_json = args.input_json or find_input_json(case_dir)
    results_dir = args.results_dir or (case_dir / "results")
    cuts_dir = results_dir / "cuts"
    solver_status = results_dir / "solver_status.json"
    logs_dir = results_dir / "logs"

    cfgs = parse_level_configs(input_json)
    rows = collect_iter_rows(cuts_dir)
    status = merge_history(rows, solver_status)
    attach_kappa_from_log(rows, logs_dir)

    stats = split_by_level(rows, cfgs)
    print(
        render_table(
            stats, case_name=case_dir.name, solver_status=status, width=args.width
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
