#!/usr/bin/env python3
"""Main entry point for pandapower to gtopt conversion."""

import argparse
import sys
from pathlib import Path

from .convert import convert

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

# Supported pandapower standard test networks (CLI name → pandapower function name)
_NETWORKS: dict[str, str] = {
    "ieee30b": "case_ieee30",
    "case4gs": "case4gs",
    "case5": "case5",
    "case6ww": "case6ww",
    "case9": "case9",
    "case14": "case14",
    "case33bw": "case33bw",
    "case57": "case57",
    "case118": "case118",
}

_DEFAULT_NETWORK = "ieee30b"

_DESCRIPTION = """\
Convert a pandapower test network to gtopt JSON format.

Reads a standard pandapower IEEE test network and writes a self-contained
gtopt JSON file ready for use with the gtopt solver or gtopt_guisrv / gtopt_websrv.
"""

_EPILOG = """
examples:
  # Convert the default IEEE 30-bus network → ieee30b.json
  pp2gtopt

  # Write to a specific output file
  pp2gtopt -o /tmp/my_case.json

  # Convert the IEEE 14-bus network
  pp2gtopt -n case14 -o ieee14b.json

  # List all available pandapower test networks
  pp2gtopt --list-networks
"""


def _list_networks_and_exit() -> None:
    """Print available networks and exit."""
    print("Available pandapower test networks:")
    for name, fn in sorted(_NETWORKS.items()):
        marker = " (default)" if name == _DEFAULT_NETWORK else ""
        print(f"  {name:<12}  pandapower.networks.{fn}(){marker}")
    sys.exit(0)


def make_parser() -> argparse.ArgumentParser:
    """Build and return the argument parser for pp2gtopt."""
    parser = argparse.ArgumentParser(
        prog="pp2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        metavar="FILE",
        default=None,
        help="output JSON file path (default: <network>.json in the current directory)",
    )
    parser.add_argument(
        "-n",
        "--network",
        metavar="NAME",
        default=_DEFAULT_NETWORK,
        choices=list(_NETWORKS),
        help=(
            "pandapower test network to convert "
            f"(default: {_DEFAULT_NETWORK}; see --list-networks)"
        ),
    )
    parser.add_argument(
        "--list-networks",
        action="store_true",
        help="list all available pandapower test networks and exit",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def main() -> None:
    """Parse arguments and run the conversion."""
    parser = make_parser()
    args = parser.parse_args()

    if args.list_networks:
        _list_networks_and_exit()

    import pandapower.networks as pn  # pylint: disable=import-outside-toplevel

    fn_name = _NETWORKS[args.network]
    net = getattr(pn, fn_name)()

    output = args.output if args.output is not None else Path(f"{args.network}.json")
    convert(output, net=net, name=args.network)


if __name__ == "__main__":
    main()
