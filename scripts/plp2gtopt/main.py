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
    args = parser.parse_args()

    options = {
        "input_dir": args.input_dir,
        "output_dir": args.output_dir,
        "output_file": args.output_file,
        "compression": "gzip",
    }

    convert_plp_case(options)


if __name__ == "__main__":
    main()
