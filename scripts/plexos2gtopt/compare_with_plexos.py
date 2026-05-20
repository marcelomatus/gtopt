#!/usr/bin/env python3
"""Compare a gtopt CEN PCP run against the PLEXOS reference solution.

Step 1 (this module) — **cost totals only**, the cheapest sanity
check.  Higher-fidelity per-element diffs (generator dispatch, bus LMP,
line flow, storage SoC) are deferred to Step 2+ once Step 1 confirms
the two runs are within a sane range.

Inputs:
  * ``--gtopt-case <dir>`` — a gtopt case directory containing
    ``output/solution.csv`` (one row per ``(scene, phase)`` with the
    ``obj_value`` column).
  * ``--plexos-log <path>`` — the PLEXOS solver log
    (``Model ( <name> ) Log.txt``) shipped inside the RES bundle's
    nested ``Solution.zip``.  Carries the ``Best Integer Solution`` /
    ``Best Bound`` lines this scout pattern-matches against.

For the CEN PCP RES bundle the log lives at
``RES{date}.zip.xz → res.zip → Model {model} Solution/Model {model}
Solution.zip → Model ( {model} ) Log.txt``.  Pass
``--plexos-res-zip <path>`` to have this scout do the two-level unzip
in a temp dir and find the log automatically.

Output: a single Rich table with both objective values, the absolute
and relative deltas, and a one-line interpretation hint.  Exit 0
always (this is a diagnostic, not a gate).
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
import zipfile
from pathlib import Path

# Rich is already a project dependency (see scripts/pyproject.toml).
from rich.console import Console
from rich.table import Table


# Matches PLEXOS log lines like
#   ``    Best Integer Solution:.....         2.8130634809e+007``
# and the matching ``Best Bound`` / ``Linear Relaxation`` lines.  The
# cost value is the last whitespace-separated token on the line.
# PLEXOS indents these report lines by 4 spaces, so we match anywhere
# in the line (no ``^`` anchor).
_LOG_LINE_RE = re.compile(
    r"(?P<label>Best (?:Integer Solution|Bound|Relaxation)):.*?(?P<value>\S+)\s*$",
    re.MULTILINE,
)


def parse_plexos_log(log_path: Path) -> dict[str, float]:
    """Pull ``Best Integer Solution`` / ``Best Bound`` / ``Linear Relaxation``
    objective values from a PLEXOS solver log.

    Returns a dict keyed by the label (e.g. ``"Best Integer Solution"``).
    Values that PLEXOS prints as ``N/A`` (no MIP relaxation, infeasible,
    etc.) are silently skipped — only numeric rows are returned.
    """
    text = log_path.read_text(encoding="utf-8", errors="replace")
    out: dict[str, float] = {}
    for m in _LOG_LINE_RE.finditer(text):
        try:
            out[m.group("label")] = float(m.group("value"))
        except ValueError:
            # ``N/A`` and friends fall through here — drop silently.
            continue
    return out


def find_plexos_log_in_res_bundle(res_zip: Path) -> Path:
    """Extract the nested log file from a CEN PCP RES bundle.

    The CEN bundle nests one solution zip inside another (and the outer
    is sometimes .zip.xz).  Returns the path to the extracted log file
    inside a ``tempfile.mkdtemp`` so the caller can read it.  No cleanup
    here — diagnostic scout, the temp dir is < 100 MB and lives in
    ``/tmp``.
    """
    # If the input is .zip.xz, decompress first.
    if res_zip.suffix == ".xz":
        import lzma

        scratch = Path(tempfile.mkdtemp(prefix="plexos_res_"))
        plain_zip = scratch / res_zip.with_suffix("").name
        with lzma.open(res_zip, "rb") as src, plain_zip.open("wb") as dst:
            dst.write(src.read())
        res_zip = plain_zip

    scratch = Path(tempfile.mkdtemp(prefix="plexos_res_inner_"))
    with zipfile.ZipFile(res_zip) as outer:
        # The CEN naming convention: ``Model <name> Solution/<...>.zip``
        # plus ``...Log.txt`` directly.  Look for the inner zip first.
        nested = next(
            (n for n in outer.namelist() if n.endswith("Solution.zip")),
            None,
        )
        if nested is None:
            # Single-zip case: the log is directly inside.
            log_name = next(
                (n for n in outer.namelist() if n.endswith("Log.txt")),
                None,
            )
            if log_name is None:
                raise FileNotFoundError(f"no Log.txt found in {res_zip}")
            outer.extract(log_name, scratch)
            return scratch / log_name

        # Two-zip case (the CEN PCP daily bundles): extract inner zip,
        # then pull the Log.txt out of it.
        outer.extract(nested, scratch)
        with zipfile.ZipFile(scratch / nested) as inner:
            log_name = next(
                (n for n in inner.namelist() if n.endswith("Log.txt")),
                None,
            )
            if log_name is None:
                raise FileNotFoundError(f"no Log.txt found inside {nested}")
            inner.extract(log_name, scratch)
            return scratch / log_name


def parse_gtopt_solution(case_dir: Path) -> dict[str, float]:
    """Read ``<case>/output/solution.csv`` and return summary objectives.

    The gtopt solution file has one row per ``(scene, phase)`` cell.
    For a single-scene single-phase run this collapses to one number.
    Returns ``{"sum_obj": ..., "max_obj": ..., "rows": ...}``.

    A missing solution.csv raises ``FileNotFoundError`` — let the
    caller decide whether to skip silently or fail.
    """
    sol_path = case_dir / "output" / "solution.csv"
    if not sol_path.exists():
        raise FileNotFoundError(
            f"no gtopt solution at {sol_path} — did the run complete?"
        )

    # Hand-rolled CSV read so this module has zero hard deps beyond
    # rich (which is already in pyproject).  Pandas would be cleaner
    # but pulls a lot in for one column.
    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    for line in sol_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        cells = [c.strip() for c in line.split(",")]
        if header is None:
            header = cells
            continue
        rows.append(dict(zip(header, cells, strict=False)))

    if not rows:
        raise ValueError(f"{sol_path}: no data rows")
    if "obj_value" not in (header or []):
        raise ValueError(f"{sol_path}: missing 'obj_value' column (have: {header})")

    obj_values = [float(r["obj_value"]) for r in rows]
    return {
        "sum_obj": sum(obj_values),
        "max_obj": max(obj_values),
        "rows": float(len(obj_values)),
    }


def _format_money(value: float) -> str:
    """Money column format: $X,XXX,XXX.XX with comma separators."""
    return f"${value:,.2f}"


def _render_report(
    plexos: dict[str, float],
    gtopt: dict[str, float] | None,
    console: Console,
) -> None:
    """Print the side-by-side cost-totals table."""
    table = Table(title="Cost totals — PLEXOS vs gtopt (Step 1 scout)")
    table.add_column("Metric", style="bold")
    table.add_column("Value", justify="right")
    table.add_column("Source")

    plexos_obj = plexos.get("Best Integer Solution")
    plexos_bound = plexos.get("Best Bound")

    if plexos_obj is not None:
        table.add_row(
            "PLEXOS MIP objective",
            _format_money(plexos_obj),
            "Best Integer Solution (log.txt)",
        )
    if plexos_bound is not None:
        table.add_row(
            "PLEXOS best bound",
            _format_money(plexos_bound),
            "Best Bound (log.txt)",
        )
    if plexos_obj and plexos_bound:
        gap = plexos_obj - plexos_bound
        rel = 100.0 * gap / abs(plexos_obj) if plexos_obj else 0.0
        table.add_row(
            "PLEXOS gap",
            f"{_format_money(gap)}  ({rel:.2f}%)",
            "computed",
        )

    if gtopt is not None:
        table.add_row(
            "gtopt sum(obj_value)",
            _format_money(gtopt["sum_obj"]),
            f"solution.csv ({int(gtopt['rows'])} rows)",
        )
        if plexos_obj:
            delta = gtopt["sum_obj"] - plexos_obj
            rel = 100.0 * delta / abs(plexos_obj)
            table.add_row(
                "Δ gtopt − PLEXOS",
                f"{_format_money(delta)}  ({rel:+.1f}%)",
                "computed",
            )

    console.print(table)

    # Interpretation hint — single line.
    if gtopt is None:
        console.print(
            "[dim]No gtopt run available yet — re-run with "
            "[bold]--gtopt-case <dir>[/bold] once a CEN PCP solve "
            "is in hand.[/dim]"
        )
        return
    if plexos_obj is None:
        return
    delta = gtopt["sum_obj"] - plexos_obj
    rel = 100.0 * delta / abs(plexos_obj)
    if abs(rel) < 2.0:
        console.print(
            "[green]Within 2% of PLEXOS MIP — looks healthy. "
            "Proceed to Step 2 (per-unit dispatch diff).[/green]"
        )
    elif delta > 0:
        console.print(
            "[yellow]gtopt costs > PLEXOS by "
            f"{rel:+.1f}%.  Likely demand_fail_cost or reserve-"
            "shortage penalty firing because a commitment or "
            "reserve constraint is overconstrained.[/yellow]"
        )
    else:
        console.print(
            f"[yellow]gtopt costs < PLEXOS by {rel:+.1f}%.  "
            "Expected when gtopt is LP-relaxed (LP ≤ MIP), but "
            "delta > 5% suggests a missing cost component "
            "(transport, emission tax, reserve cost).[/yellow]"
        )


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="compare_with_plexos",
        description=(
            "Cost-totals scout (Step 1) for PLEXOS vs gtopt on the CEN PCP daily case."
        ),
    )
    parser.add_argument(
        "--gtopt-case",
        type=Path,
        default=None,
        help="gtopt case directory (contains output/solution.csv)",
    )
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument(
        "--plexos-log",
        type=Path,
        default=None,
        help="path to a PLEXOS solver log file (Log.txt)",
    )
    src.add_argument(
        "--plexos-res-zip",
        type=Path,
        default=None,
        help=(
            "path to a CEN PCP RES bundle (RES*.zip or RES*.zip.xz); "
            "the scout extracts the nested log automatically"
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    console = Console()

    log_path: Path
    if args.plexos_log is not None:
        log_path = args.plexos_log
    else:
        log_path = find_plexos_log_in_res_bundle(args.plexos_res_zip)
        console.print(f"[dim]Extracted PLEXOS log: {log_path}[/dim]")

    plexos = parse_plexos_log(log_path)
    if not plexos:
        console.print(
            f"[red]No objective lines parsed from {log_path} — "
            "check the file format.[/red]"
        )
        return 1

    gtopt: dict[str, float] | None = None
    if args.gtopt_case is not None:
        try:
            gtopt = parse_gtopt_solution(args.gtopt_case)
        except (FileNotFoundError, ValueError) as exc:
            console.print(f"[yellow]gtopt side unavailable: {exc}[/yellow]")

    _render_report(plexos, gtopt, console)
    return 0


if __name__ == "__main__":
    sys.exit(main())
