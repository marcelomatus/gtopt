# SPDX-License-Identifier: BSD-3-Clause
"""CLI entry point for gtopt_results_summary."""

from __future__ import annotations

import argparse
import json
import sys

from .summary import summarize_results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="gtopt_results_summary",
        description=(
            "Produce a KPI summary (JSON) of a gtopt optimization "
            "result directory or ZIP."
        ),
    )
    parser.add_argument(
        "source",
        help="Path to the output directory or a results .zip file.",
    )
    parser.add_argument(
        "--scale-objective",
        type=float,
        default=1.0,
        help=(
            "Value of options.scale_objective used when the case was solved. "
            "The raw objective is multiplied by this to produce total cost."
        ),
    )
    parser.add_argument(
        "--tech-map",
        default=None,
        help=(
            "Optional path to a JSON file mapping generator UIDs (as strings) "
            "to a technology label (hydro/solar/wind/thermal/battery/other)."
        ),
    )
    parser.add_argument(
        "--pretty",
        action="store_true",
        help="Pretty-print the JSON output with indent=2.",
    )
    args = parser.parse_args(argv)

    tech_map = None
    if args.tech_map:
        with open(args.tech_map, "r", encoding="utf-8") as fh:
            tech_map = json.load(fh)

    summary = summarize_results(
        args.source,
        tech_map=tech_map,
        scale_objective=args.scale_objective,
    )
    indent = 2 if args.pretty else None
    json.dump(summary, sys.stdout, indent=indent, default=str)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
