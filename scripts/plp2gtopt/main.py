#!/usr/bin/env python3
"""Main entry point for PLP to GTOPT conversion."""

import argparse
import logging
import signal
import sys
from pathlib import Path

from .plp2gtopt import convert_plp_case

try:
    from importlib.metadata import version as _pkg_version, PackageNotFoundError

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

_LOG_LEVEL_CHOICES = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]

_DESCRIPTION = """\
Convert a PLP (PLPMAX/PLPOPT) case directory to gtopt JSON format.

Reads the standard PLP data files (plpblo.dat, plpbar.dat, plpcosce.dat,
plpcnfce.dat, plpcnfli.dat, plpdem.dat, plpeta.dat, …) from INPUT_DIR and
writes a self-contained gtopt JSON file together with Parquet time-series
files to OUTPUT_DIR.
"""

_EPILOG = """
examples:
  # Convert a PLP case in the current directory (uses ./input → ./output)
  plp2gtopt

  # Specify directories explicitly
  plp2gtopt -i /data/plp_case -o /data/gtopt_case

  # Generate a ZIP archive compatible with gtopt_guisrv / gtopt_websrv
  plp2gtopt -z -i plp_case_2y -o gtopt_case_2y

  # Limit conversion to the first 5 stages
  plp2gtopt -i input/ -s 5

  # Two hydrology scenarios with explicit probability weights
  plp2gtopt -i input/ -y 1,2 -p 0.6,0.4

  # Apply a 10% annual discount rate
  plp2gtopt -i input/ -d 0.10

  # Show verbose debug output
  plp2gtopt -i input/ -l DEBUG
"""


def signal_handler(sig, _frame):
    """Handle termination signals gracefully."""
    signame = signal.strsignal(sig)
    print(f"\nCaught signal {signame}. Exiting...")
    sys.exit(0)


def make_parser() -> argparse.ArgumentParser:
    """Build and return the argument parser for plp2gtopt."""
    parser = argparse.ArgumentParser(
        prog="plp2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )
    parser.add_argument(
        "-i",
        "--input-dir",
        type=Path,
        metavar="DIR",
        default=Path("input"),
        help="directory containing PLP input files (default: %(default)s)",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        metavar="DIR",
        default=Path("output"),
        help="directory for gtopt output files (default: %(default)s)",
    )
    parser.add_argument(
        "-f",
        "--output-file",
        type=Path,
        metavar="FILE",
        default=None,
        help=(
            "output JSON file path "
            "(default: <output-dir-name>.json in the current directory)"
        ),
    )
    parser.add_argument(
        "-s",
        "--last-stage",
        dest="last_stage",
        type=int,
        metavar="N",
        default=-1,
        help="last stage to include (default: all stages)",
    )
    parser.add_argument(
        "-d",
        "--discount-rate",
        dest="discount_rate",
        type=float,
        metavar="RATE",
        default=0.0,
        help="annual discount rate, e.g. 0.10 for 10%% (default: %(default)s)",
    )
    parser.add_argument(
        "-m",
        "--management-factor",
        dest="management_factor",
        type=float,
        metavar="FACTOR",
        default=0.0,
        help="demand management factor (default: %(default)s)",
    )
    parser.add_argument(
        "-t",
        "--last-time",
        dest="last_time",
        type=float,
        metavar="T",
        default=-1,
        help="last time value to extract (default: all time steps)",
    )
    parser.add_argument(
        "-n",
        "--name",
        dest="name",
        metavar="NAME",
        default=None,
        help=(
            "name for the system in the output JSON "
            "(default: basename of the output JSON file)"
        ),
    )
    parser.add_argument(
        "--sys-version",
        dest="sys_version",
        metavar="VERSION",
        default="",
        help="version string for the system in the output JSON (default: empty)",
    )
    parser.add_argument(
        "-F",
        "--output-format",
        dest="output_format",
        metavar="FORMAT",
        default="parquet",
        choices=["parquet", "csv"],
        help="output file format: parquet or csv (default: %(default)s)",
    )
    parser.add_argument(
        "--input-format",
        dest="input_format",
        metavar="FORMAT",
        default=None,
        choices=["parquet", "csv"],
        help=(
            "input format for gtopt to read time-series files "
            "(default: same as output-format)"
        ),
    )
    parser.add_argument(
        "-c",
        "--compression",
        dest="compression",
        metavar="ALG",
        default="gzip",
        help="compression codec for output files (default: %(default)s)",
    )
    parser.add_argument(
        "-y",
        "--hydrologies",
        dest="hydrologies",
        metavar="H1[,H2,…]",
        default="0",
        help="comma-separated hydrology scenario indices (default: %(default)s)",
    )
    parser.add_argument(
        "-p",
        "--probability-factors",
        dest="probability_factors",
        metavar="P1[,P2,…]",
        default=None,
        help=(
            "comma-separated probability weights for each hydrology scenario "
            "(default: equal distribution)"
        ),
    )
    parser.add_argument(
        "-z",
        "--zip",
        dest="zip_output",
        action="store_true",
        default=False,
        help=(
            "create a ZIP archive <output-dir>.zip containing the JSON file "
            "and all Parquet/CSV data files (compatible with gtopt_guisrv and "
            "gtopt_websrv)"
        ),
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=_LOG_LEVEL_CHOICES,
        metavar="LEVEL",
        help=(
            "logging verbosity: DEBUG, INFO, WARNING, ERROR, CRITICAL "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def build_options(args: argparse.Namespace) -> dict:
    """Convert parsed CLI arguments to a conversion options dict."""
    output_file = args.output_file
    if output_file is None:
        output_file = Path(args.output_dir.name).with_suffix(".json")
    name = args.name if args.name is not None else Path(output_file).stem
    input_format = args.input_format if args.input_format else args.output_format
    return {
        "input_dir": args.input_dir,
        "output_dir": args.output_dir,
        "output_file": output_file,
        "last_stage": args.last_stage,
        "last_time": args.last_time,
        "compression": args.compression,
        "output_format": args.output_format,
        "input_format": input_format,
        "hydrologies": args.hydrologies,
        "probability_factors": args.probability_factors,
        "discount_rate": args.discount_rate,
        "management_factor": args.management_factor,
        "zip_output": args.zip_output,
        "name": name,
        "sys_version": args.sys_version,
    }


def main():
    """Parse arguments and initiate conversion."""
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    parser = make_parser()
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    convert_plp_case(build_options(args))


if __name__ == "__main__":
    main()
