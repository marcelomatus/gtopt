# SPDX-License-Identifier: BSD-3-Clause
"""CLI for gtopt_check_topology -- preflight network topology analyser.

Usage
-----
::

    python -m gtopt_check_topology PLEXOS20260407.json
    python -m gtopt_check_topology PLEXOS20260407.json --json-out /tmp/topo.json
    python -m gtopt_check_topology PLEXOS20260407.json \\
        --strict-direction-lines /tmp/sd.json
    python -m gtopt_check_topology PLEXOS20260407.json --quiet

Exit codes
----------
0  -- the planning file parsed and no fatal data error was found
1  -- the planning file could not be read / parsed
2  -- a fatal data error was found (negative reactance, island without
      generator, ...).  Warnings (dangerous cycles, bridges, voltage mix)
      do NOT trigger a non-zero exit so this tool stays safe to use in
      pre-flight pipelines.
"""

from __future__ import annotations

import argparse
import json
import sys
import warnings
from pathlib import Path
from typing import Any

from rich.console import Console

from gtopt_check_topology._analyzer import (
    DEFAULT_DANGER_THRESHOLD,
    DEFAULT_MAX_CYCLE_SIZE,
    analyse_planning,
)
from gtopt_check_topology._render import (
    render_console_report,
    report_to_json,
    sos1_candidates_payload,
    write_json,
)


def _load_planning(path: Path) -> dict[str, Any]:
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, dict):
        raise ValueError(f"{path}: top-level JSON is not an object")
    return data


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="gtopt_check_topology",
        description=(
            "Preflight network-topology analysis on a gtopt planning JSON: "
            "islands, fundamental cycles with per-cycle danger score, "
            "bridges, DC links, negative R/X, voltage-mixing."
        ),
    )
    parser.add_argument(
        "planning_json",
        type=Path,
        help="Path to the gtopt planning JSON (e.g. PLEXOS20260407.json).",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        default=None,
        metavar="PATH",
        help="Emit the full machine-readable report as JSON at PATH.",
    )
    parser.add_argument(
        "--strict-direction-lines",
        type=Path,
        default=None,
        metavar="PATH",
        help=(
            "Emit the strict-direction candidate list (uid + name + bus pair) "
            "at PATH in the format the converter will consume.  These are the "
            "lines sitting on dangerous KVL loops where the LP is "
            "geometrically forced to pick a single direction."
        ),
    )
    parser.add_argument(
        "--emit-sos1",
        type=Path,
        default=None,
        metavar="PATH",
        help=(
            "[DEPRECATED] Legacy alias of --strict-direction-lines.  "
            "Will be removed in a future release."
        ),
    )
    parser.add_argument(
        "--danger-threshold",
        type=int,
        default=DEFAULT_DANGER_THRESHOLD,
        help=(
            "Cycle danger-score cut-off (default %(default)s).  Cycles with "
            "score >= threshold are flagged dangerous and contribute to the "
            "SOS1 candidate list."
        ),
    )
    parser.add_argument(
        "--max-cycle-size",
        type=int,
        default=DEFAULT_MAX_CYCLE_SIZE,
        help=(
            "Only consider fundamental cycles with <= N buses (default "
            "%(default)s).  Larger means more expensive cycle_basis on "
            "dense networks."
        ),
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Only show flagged categories (suppress OK lines).",
    )
    parser.add_argument(
        "--include-benign-loops",
        action="store_true",
        help="List ALL cycles, not just the dangerous ones.",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="Disable ANSI colour output (useful in pipelines / CI logs).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    planning_path: Path = args.planning_json
    if not planning_path.exists():
        print(f"Error: file not found: {planning_path}", file=sys.stderr)
        return 1

    try:
        planning = _load_planning(planning_path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"Error reading {planning_path}: {exc}", file=sys.stderr)
        return 1

    # Thread the planning file's parent directory through so the analyser
    # can locate side-car .pampl user-constraint files (D3 scans them).
    planning["_pampl_dir"] = str(planning_path.parent)

    report = analyse_planning(
        planning,
        bundle=planning_path.name,
        danger_threshold=args.danger_threshold,
        max_cycle_size=args.max_cycle_size,
    )

    console = Console(
        no_color=args.no_color,
        force_terminal=False if args.no_color else None,
    )
    render_console_report(
        report,
        console=console,
        quiet=args.quiet,
        include_benign_loops=args.include_benign_loops,
    )

    if args.json_out is not None:
        write_json(report_to_json(report), str(args.json_out))
        if not args.quiet:
            console.print(f"\nWrote full report -> {args.json_out}")

    # Resolve --strict-direction-lines / --emit-sos1 (the latter is a
    # deprecated alias).  Emitting both is allowed -- the new flag wins
    # if both are passed, but each path is honoured independently.
    strict_path: Path | None = args.strict_direction_lines
    if args.emit_sos1 is not None:
        warnings.warn(
            "--emit-sos1 is deprecated; use --strict-direction-lines instead.",
            DeprecationWarning,
            stacklevel=2,
        )
        if strict_path is None:
            strict_path = args.emit_sos1
        else:
            # Both were passed -- still honour the legacy path for
            # backward-compat consumers, writing the same payload.
            write_json(sos1_candidates_payload(report), str(args.emit_sos1))
            if not args.quiet:
                console.print(
                    f"Wrote strict-direction candidates -> {args.emit_sos1} "
                    "(via deprecated --emit-sos1)"
                )

    if strict_path is not None:
        write_json(sos1_candidates_payload(report), str(strict_path))
        if not args.quiet:
            console.print(f"Wrote strict-direction candidates -> {strict_path}")

    # Fatal data errors trigger exit 2; warnings keep exit 0.
    if report.negative_impedance or report.islands_no_generator:
        return 2
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised via CLI
    raise SystemExit(main())
