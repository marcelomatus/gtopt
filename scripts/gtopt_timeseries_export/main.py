# SPDX-License-Identifier: BSD-3-Clause
"""CLI entry point for gtopt_timeseries_export."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .exporter import export_to_excel


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="gtopt_timeseries_export",
        description=(
            "Export a gtopt output directory or results ZIP to a single "
            "Excel (.xlsx) workbook with an Overview sheet and one sheet "
            "per output table."
        ),
    )
    parser.add_argument(
        "source",
        help="Path to the output directory or a results .zip file.",
    )
    parser.add_argument(
        "-o",
        "--output",
        default=None,
        help="Destination .xlsx path (default: <source>.xlsx).",
    )
    parser.add_argument(
        "--scale-objective",
        type=float,
        default=1.0,
        help="Value of options.scale_objective used when the case was solved.",
    )
    parser.add_argument(
        "--tech-map",
        default=None,
        help="Optional JSON file mapping generator UIDs to technology labels.",
    )
    args = parser.parse_args(argv)

    tech_map = None
    if args.tech_map:
        with open(args.tech_map, "r", encoding="utf-8") as fh:
            tech_map = json.load(fh)

    src = Path(args.source)
    dest = (
        Path(args.output)
        if args.output
        else src.with_suffix(".xlsx")
        if src.suffix
        else src.parent / f"{src.name}.xlsx"
    )

    result = export_to_excel(
        src, dest, scale_objective=args.scale_objective, tech_map=tech_map
    )
    sys.stdout.write(f"Wrote: {result}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
