#!/usr/bin/env python3
"""Main entry point for PLP to GTOPT conversion."""

import argparse
import signal
import sys
from pathlib import Path
from .plp2gtopt import convert_plp_case


def signal_handler(sig, _frame):
    """Handle termination signals gracefully."""
    signame = signal.strsignal(sig)
    print(f"\nCaught signal {signame}. Exiting...")
    sys.exit(0)


def main():
    """Parse arguments and initiate conversion."""
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    parser = argparse.ArgumentParser(
        description="Convert PLP input files to GTOPT format"
    )
    parser.add_argument(
        "-i",
        "--input-dir",
        type=Path,
        help="directory containing PLP input files (default: input)",
        default=Path("input"),
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        help="directory to write GTOPT output files (default: output)",
        default=Path("output"),
    )
    parser.add_argument(
        "-f",
        "--output-file",
        type=Path,
        help="file to write the GTOPT output (default: output/gtopt_case.json)",
        default=Path("output/gtopt_case.json"),
    )
    parser.add_argument(
        "-s",
        "--last-stage",
        dest="last_stage",
        type=int,
        help="last stage number to extract from plp data files (default: all stages)",
        default=-1,
    )
    parser.add_argument(
        "-d",
        "--discount-rate",
        dest="discount_rate",
        type=float,
        help="annual discount rate",
        default=0.0,
    )
    parser.add_argument(
        "-m",
        "--management-factor",
        dest="management_factor",
        type=float,
        help="demand management factor",
        default=0.0,
    )
    parser.add_argument(
        "-t",
        "--last-time",
        dest="last_time",
        type=float,
        help="last time to extract from plp data files (default: all times)",
        default=-1,
    )
    parser.add_argument(
        "-c",
        "--compression",
        dest="compression",
        type=str,
        help="compression format for output files (default: gzip)",
        default="gzip",
    )
    parser.add_argument(
        "-y",
        "--hydrologies",
        dest="hydrologies",
        type=str,
        help="comma-separated list of hydrology scenarios (default: 1)",
        default="0",
    )
    parser.add_argument(
        "-p",
        "--probability-factors",
        dest="probability_factors",
        type=str,
        help="comma-separated list of probability factors for each hydrology scenario "
        "(default: equal distribution)",
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
        "probability_factors": args.probability_factors,
        "discount_rate": args.discount_rate,
        "management_factor": args.management_factor,
    }

    convert_plp_case(options)


if __name__ == "__main__":
    main()
