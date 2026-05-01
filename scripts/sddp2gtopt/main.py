#!/usr/bin/env python3
"""CLI entry-point for ``sddp2gtopt``.

Mirrors :mod:`plp2gtopt.main` where it makes sense, dropping
PLP-specific flags (``--plp-legacy``, ``--pmin-as-flowright``,
``--reservoir-energy-scale``, …). v0 wires only ``--info`` and
``--validate``; the conversion path is stubbed in
:func:`sddp2gtopt.sddp2gtopt.convert_sddp_case` and will land in P1.
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
from pathlib import Path

from .info_display import display_sddp_info
from .sddp2gtopt import convert_sddp_case, validate_sddp_case


__version__ = "0.1.0"


_DESCRIPTION = """\
Convert a PSR SDDP case directory to gtopt JSON format.

v0 reads the typed snapshot ``psrclasses.json`` that PSR SDDP writes
alongside the legacy ``.dat`` files. ``--info`` and ``--validate``
are wired today; full conversion is in development (see DESIGN.md).
"""

_EPILOG = """\
Examples:
  sddp2gtopt --info  case0           Show a summary of the case
  sddp2gtopt --validate case0        Light schema sanity check
  sddp2gtopt -i case0 -o gtopt_case0 (P1+) Convert to gtopt JSON
"""


def _signal_handler(sig: int, _frame: object) -> None:
    """Terminate cleanly on SIGINT/SIGTERM."""
    print(f"\nCaught signal {signal.strsignal(sig)}. Exiting...")
    sys.exit(0)


def make_parser() -> argparse.ArgumentParser:
    """Build the argparse parser for ``sddp2gtopt``."""
    parser = argparse.ArgumentParser(
        prog="sddp2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )
    parser.add_argument(
        "positional_input",
        nargs="?",
        type=Path,
        default=None,
        help="SDDP case directory (alternative to -i)",
    )
    parser.add_argument(
        "-i",
        "--input-dir",
        type=Path,
        default=None,
        help="SDDP case directory containing psrclasses.json",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=None,
        help="output directory for the gtopt planning (P1+)",
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"),
        help="logging level (default: INFO)",
    )
    parser.add_argument(
        "--info",
        dest="show_info",
        action="store_true",
        help="show a summary of the case and exit",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="run schema sanity checks and exit (0 = ok)",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def _resolve_input_dir(args: argparse.Namespace) -> Path | None:
    """Pick ``-i`` over the positional argument; ``None`` if neither set."""
    if args.input_dir is not None:
        return args.input_dir
    if args.positional_input is not None:
        return args.positional_input
    return None


def main(argv: list[str] | None = None) -> None:
    """Parse arguments and dispatch to the right action."""
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    parser = make_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    input_dir = _resolve_input_dir(args)
    if input_dir is None:
        parser.error("input directory is required: pass it positionally or via -i")

    options = {"input_dir": input_dir, "output_dir": args.output_dir}

    if args.show_info:
        try:
            display_sddp_info(options)
        except (FileNotFoundError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            sys.exit(1)
        return

    if args.validate:
        sys.exit(0 if validate_sddp_case(options) else 1)

    # Default action: convert. v0 raises NotImplementedError loudly;
    # P1 will return a CRITICAL-finding count instead.
    try:
        critical = convert_sddp_case(options) or 0
    except NotImplementedError as exc:
        print(f"sddp2gtopt: {exc}", file=sys.stderr)
        sys.exit(2)
    except (FileNotFoundError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

    if critical > 0:
        print(
            f"error: conversion completed with {critical} CRITICAL "
            "finding(s) — fix the underlying issue before using the output.",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
