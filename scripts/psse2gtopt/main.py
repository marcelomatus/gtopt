#!/usr/bin/env python3
"""CLI entry-point for ``psse2gtopt``.

Mirrors :mod:`sddp2gtopt.main` / :mod:`plp2gtopt.main` where it makes
sense.  PSS/E carries only network + power-flow data (no costs), so the
PSS/E-specific flags are economic knobs (``--gcost``,
``--demand-fail-cost``) plus the ``--raw`` snapshot selector and a
``--single-bus`` copper-plate fallback.
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path

from gtopt_config import add_color_argument
from gtopt_shared.cli_signals import (
    install_termination_handlers,
    signal_handler as _signal_handler,  # noqa: F401  re-export for tests
)

from .info_display import display_psse_info
from .psse2gtopt import convert_psse_case, validate_psse_case


__version__ = "0.1.0"


_DESCRIPTION = """\
Convert a PSS(R)E RAW power-flow case to gtopt JSON format.

Reads the classic comma-separated PSS/E ``.raw`` (revisions 32/33) and
emits a single-snapshot multi-bus DC OPF planning that ``gtopt
--lp-only`` ingests directly: buses -> Bus, branches + transformers ->
Line (per-unit reactance; 3-winding transformers expand to a star bus),
generators -> Generator, loads -> Demand.

PSS/E RAW files hold no economic data, so every generator is assigned a
single uniform ``--gcost``; tune ``--demand-fail-cost`` for the
unserved-energy penalty.  ``--info`` prints a case summary and
``--validate`` runs a light sanity check.
"""

_EPILOG = """\
Examples:
  psse2gtopt --info  case.raw              Show a summary of the case
  psse2gtopt --info  ./PSS(R)Ev33         Summarise the default .raw in a dir
  psse2gtopt --validate case.raw           Light sanity check
  psse2gtopt -i case.raw -o gtopt_case     Convert to gtopt JSON
  psse2gtopt ./PSS(R)Ev33 --raw PAESEPMED  Pick one snapshot from a directory
"""


def make_parser() -> argparse.ArgumentParser:
    """Build the argparse parser for ``psse2gtopt``."""
    parser = argparse.ArgumentParser(
        prog="psse2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )
    parser.add_argument(
        "positional_input",
        nargs="?",
        type=Path,
        default=None,
        help="PSS/E .raw file or a directory containing one (alternative to -i)",
    )
    parser.add_argument(
        "-i",
        "--input",
        dest="input",
        type=Path,
        default=None,
        help="PSS/E .raw file or directory of snapshots",
    )
    parser.add_argument(
        "--raw",
        default=None,
        help="when the input is a directory, substring selecting one .raw",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=None,
        help="output directory for the gtopt planning JSON",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="planning name (default: the .raw file stem)",
    )
    parser.add_argument(
        "--gcost",
        type=float,
        default=10.0,
        help="base generation cost [$/MWh] (uniform, or rank-0 cost with --ldm; default: 10)",
    )
    parser.add_argument(
        "--gcost-step",
        type=float,
        default=1.0,
        help="per-merit-rank cost increment [$/MWh] when --ldm is given (default: 1)",
    )
    parser.add_argument(
        "--rating-set",
        choices=("A", "B", "C"),
        default="A",
        help="line/transformer rating set: A=normal (default), B/C=emergency",
    )
    parser.add_argument(
        "--nomenclatura",
        type=Path,
        default=None,
        help="Nomenclatura .xls/.xlsx for human-readable bus names",
    )
    parser.add_argument(
        "--ldm",
        type=Path,
        default=None,
        help="LDM (Lista de Mérito) .xlsx to drive rank-based merit-order gcost",
    )
    parser.add_argument(
        "--demand-fail-cost",
        type=float,
        default=1000.0,
        help="unserved-energy penalty [$/MWh] (default: 1000)",
    )
    parser.add_argument(
        "--scale-objective",
        type=float,
        default=1000.0,
        help="objective scaling divisor for numerics (default: 1000)",
    )
    parser.add_argument(
        "--single-bus",
        action="store_true",
        help="emit a copper-plate single-bus planning (no lines) as a fallback",
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
        help="run sanity checks and exit (0 = ok)",
    )
    add_color_argument(parser)
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def _resolve_input(args: argparse.Namespace) -> Path | None:
    """Pick ``-i`` over the positional argument; ``None`` if neither set."""
    if args.input is not None:
        return args.input
    if args.positional_input is not None:
        return args.positional_input
    return None


def main(argv: list[str] | None = None) -> None:
    """Parse arguments and dispatch to the right action."""
    install_termination_handlers()

    parser = make_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    input_path = _resolve_input(args)
    if input_path is None:
        parser.error("input is required: pass it positionally or via -i")

    options = {
        "input": input_path,
        "raw": args.raw,
        "output_dir": args.output_dir,
        "name": args.name,
        "gcost": args.gcost,
        "gcost_step": args.gcost_step,
        "demand_fail_cost": args.demand_fail_cost,
        "scale_objective": args.scale_objective,
        "single_bus": args.single_bus,
        "rating_set": args.rating_set,
        "nomenclatura": args.nomenclatura,
        "ldm": args.ldm,
    }

    if args.show_info:
        try:
            display_psse_info(options)
        except (FileNotFoundError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            sys.exit(1)
        return

    if args.validate:
        sys.exit(0 if validate_psse_case(options) else 1)

    try:
        critical = convert_psse_case(options) or 0
    except (FileNotFoundError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

    if critical > 0:
        print(
            f"error: conversion completed with {critical} CRITICAL finding(s).",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
