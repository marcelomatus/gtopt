#!/usr/bin/env python3
"""Main entry point for PLP to GTOPT conversion."""

import argparse
from pathlib import Path
from .plp2gtopt import convert_plp_case


def main():
    parser = argparse.ArgumentParser(description="Convert PLP input files to GTOPT format")
    parser.add_argument("input_dir", type=Path, help="Directory containing PLP input files")
    parser.add_argument("output_dir", type=Path, help="Directory to write GTOPT output files")
    args = parser.parse_args()

    convert_plp_case(args.input_dir, args.output_dir)


if __name__ == "__main__":
    main()
