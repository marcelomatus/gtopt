#!/usr/bin/env python3
"""Main entry point for pandapower to gtopt conversion."""

import argparse
import logging
import sys
from pathlib import Path
from typing import Any

from .convert import _SUPPORTED_FORMATS, convert, load_network

logger = logging.getLogger(__name__)

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

_DESCRIPTION = f"""\
Convert a pandapower network to gtopt JSON format.

Input sources (mutually exclusive):
  -f FILE   Load a pandapower network file ({_SUPPORTED_FORMATS}).
  -n NAME   Use a built-in pandapower test network (see --list-networks).
            Default: {_DEFAULT_NETWORK}

Writes a self-contained gtopt JSON file ready for the gtopt solver,
gtopt_guisrv, or gtopt_websrv.
"""

_EPILOG = """
examples:
  # Convert the default IEEE 30-bus built-in network → ieee30b.json
  pp2gtopt

  # Convert a saved pandapower JSON file
  pp2gtopt -f my_network.json -o my_case.json

  # Convert a MATPOWER case file
  pp2gtopt -f case39.m -o case39.json

  # Convert a pandapower Excel workbook
  pp2gtopt -f network.xlsx -o network.json

  # Use a specific built-in test network
  pp2gtopt -n case14 -o ieee14b.json

  # List all available built-in networks
  pp2gtopt --list-networks
"""


def _list_networks_and_exit() -> None:
    """Print available networks and exit."""
    print("Available pandapower test networks:")
    for name, fn in sorted(_NETWORKS.items()):
        marker = " (default)" if name == _DEFAULT_NETWORK else ""
        print(f"  {name:<12}  pandapower.networks.{fn}(){marker}")
    sys.exit(0)


def _log_element_counts(planning: dict[str, Any]) -> None:
    """Print element counts from the generated gtopt planning dict."""
    try:
        from gtopt_check_json._info import print_info  # noqa: PLC0415

        print_info(planning)
    except ImportError:
        # Fallback: plain logger output when _terminal is not available
        sys_data = planning.get("system", {})
        sim = planning.get("simulation", {})
        logger.info("=== Generated gtopt element counts ===")
        logger.info("  System name     : %s", sys_data.get("name", "(unnamed)"))
        logger.info("  Buses           : %d", len(sys_data.get("bus_array", [])))
        logger.info("  Generators      : %d", len(sys_data.get("generator_array", [])))
        logger.info("  Demands         : %d", len(sys_data.get("demand_array", [])))
        logger.info("  Lines           : %d", len(sys_data.get("line_array", [])))
        logger.info("  Blocks          : %d", len(sim.get("block_array", [])))
        logger.info("  Stages          : %d", len(sim.get("stage_array", [])))
        logger.info("  Scenarios       : %d", len(sim.get("scenario_array", [])))


def run_post_check(planning: dict[str, Any]) -> None:
    """Run gtopt_check_json validation on the generated planning dict.

    Prints styled element counts and system info, and if gtopt_check_json
    is available, runs run_all_checks().  Skips gracefully if
    gtopt_check_json is not installed.

    Parameters
    ----------
    planning
        The planning dict produced by convert().
    """
    _log_element_counts(planning)

    try:
        from gtopt_check_json._checks import (  # noqa: PLC0415
            run_all_checks,
            Severity,
        )
        from gtopt_check_json._terminal import (  # noqa: PLC0415
            print_finding as _pf,
            print_status,
            print_summary,
        )
    except ImportError:
        logger.debug("gtopt_check_json not available; skipping JSON validation checks")
        return

    findings = run_all_checks(planning, enabled_checks=None, ai_options=None)

    if not findings:
        print_status("All checks passed — no issues found.", ok=True)
        return

    critical_count = 0
    warning_count = 0
    note_count = 0
    for finding in findings:
        _pf(finding.severity.name, finding.check_id, finding.message)
        if finding.severity == Severity.CRITICAL:
            critical_count += 1
        elif finding.severity == Severity.WARNING:
            warning_count += 1
        else:
            note_count += 1

    print_summary(critical_count, warning_count, note_count)


def make_parser() -> argparse.ArgumentParser:
    """Build and return the argument parser for pp2gtopt."""
    parser = argparse.ArgumentParser(
        prog="pp2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )

    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "-f",
        "--file",
        type=Path,
        metavar="FILE",
        default=None,
        help=(f"pandapower network file to convert (supported: {_SUPPORTED_FORMATS})"),
    )
    source.add_argument(
        "-n",
        "--network",
        metavar="NAME",
        default=None,
        choices=list(_NETWORKS),
        help=(
            "built-in pandapower test network to convert "
            f"(default: {_DEFAULT_NETWORK}; see --list-networks)"
        ),
    )

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        metavar="FILE",
        default=None,
        help="output JSON file path (default: <stem>.json in the current directory)",
    )
    parser.add_argument(
        "--list-networks",
        action="store_true",
        help="list all available built-in pandapower test networks and exit",
    )
    parser.add_argument(
        "--check",
        dest="run_check",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "run post-conversion validation via gtopt_check_json: prints "
            "element counts and basic consistency checks. "
            "Use --no-check to disable. (default: enabled)"
        ),
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
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    # Use clean formatter for non-DEBUG levels
    try:
        from gtopt_check_json._terminal import CleanFormatter  # noqa: PLC0415

        for handler in logging.getLogger().handlers:
            handler.setFormatter(CleanFormatter())
    except ImportError:
        pass

    parser = make_parser()
    args = parser.parse_args()

    if args.list_networks:
        _list_networks_and_exit()

    if args.file is not None:
        net = load_network(args.file)
        name = args.file.stem
    else:
        import pandapower.networks as pn  # pylint: disable=import-outside-toplevel

        network = args.network if args.network is not None else _DEFAULT_NETWORK
        fn_name = _NETWORKS[network]
        net = getattr(pn, fn_name)()
        name = network

    output = args.output if args.output is not None else Path(f"{name}.json")
    planning = convert(output, net=net, name=name)

    if args.run_check:
        run_post_check(planning)


if __name__ == "__main__":
    main()
