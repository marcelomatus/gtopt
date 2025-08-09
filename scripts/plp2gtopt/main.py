#!/usr/bin/env python3
"""Main entry point for PLP to GTOPT conversion."""

import argparse
from pathlib import Path
from .plp2gtopt import convert_plp_case


def main():
    """Parse arguments and initiate conversion."""
    parser = argparse.ArgumentParser(
        description="Convert PLP input files to GTOPT format"
    )
    parser.add_argument(
        "input_dir",
        type=Path,
        help="Directory containing PLP input files",
        default=Path("input"),
    )
    parser.add_argument(
        "output_dir",
        type=Path,
        help="Directory to write GTOPT output files",
        default=Path("output"),
    )
    parser.add_argument(
        "output_file",
        type=Path,
        help="File to write the GTOPT output",
        default=Path("output/gtopt_case.json"),
    )
    parser.add_argument(
        "--last-stage",
        dest="last_stage",
        type=int,
        help="Last stage number to extract from the plp data files (default: all stages)",
        default=-1,
    )
    parser.add_argument(
        "--last-time",
        dest="last_time",
        type=float,
        help="Last time to extract from the plp data files (default: all times)",
        default=-1,
    )
    parser.add_argument(
        "--compression",
        dest="compression",
        type=str,
        help="Compression format for output files (default: gzip)",
        default="gzip",
    )
    parser.add_argument(
        "--hydrologies",
        dest="hydrologies",
        type=str,
        help="Comma-separated list of hydrology scenarios (default: 1)",
        default="1",
    )
    parser.add_argument(
        "--probability-factors",
        dest="probability_factors",
        type=str,
        help="Comma-separated list of probability factors for each hydrology"
        " scenario (default: equal distribution)",
        default=None,
    )
    args = parser.parse_args()

    options = {
        "input_dir": args.input_dir,
        "output_dir": args.output_dir,
        "output_file": args.output_file,
        "last_stage": args.last_stage,
        "last_time": args.last_time,
        "compression": args.compression,
        "hydrologies": args.hydrologies,
    }

    convert_plp_case(options)


if __name__ == "__main__":
    main()
