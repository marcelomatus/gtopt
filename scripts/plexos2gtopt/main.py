#!/usr/bin/env python3
"""CLI entry-point for ``plexos2gtopt``.

Mirrors :mod:`sddp2gtopt.main` where it makes sense: ``--info`` and
``--validate`` for quick inspection, the no-flag default for full
conversion to a gtopt planning JSON.
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
from pathlib import Path

from .info_display import display_plexos_info
from .plexos2gtopt import convert_plexos_bundle, validate_plexos_bundle


__version__ = "0.1.0"


_DESCRIPTION = """\
Convert a CEN PCP daily PLEXOS bundle to gtopt JSON format.

v0 reads the PLEXOS XML object database (``DBSEN_PRGDIARIO.xml``) plus
the per-class CSV time-series shipped in ``DATOS{date}.zip``. ``--info``
and ``--validate`` are wired today; full conversion is in development
(see DESIGN.md).
"""

_EPILOG = """\
Examples:
  plexos2gtopt --info  support/plexos_pcp_2026-04-22/DATOS20260422.zip.xz
  plexos2gtopt --validate support/plexos_pcp_2026-04-22
  plexos2gtopt -i support/plexos_pcp_2026-04-22/PLEXOS20260422.zip -o gtopt_PLEXOS20260422
"""


def _signal_handler(sig: int, _frame: object) -> None:
    """Terminate cleanly on SIGINT/SIGTERM."""
    print(f"\nCaught signal {signal.strsignal(sig)}. Exiting...")
    sys.exit(0)


def make_parser() -> argparse.ArgumentParser:
    """Build the argparse parser for ``plexos2gtopt``."""
    parser = argparse.ArgumentParser(
        prog="plexos2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )
    parser.add_argument(
        "positional_input",
        nargs="?",
        type=Path,
        default=None,
        help="PLEXOS bundle path: a directory, DATOS*.zip[.xz], or PLEXOS*.zip",
    )
    parser.add_argument(
        "-i",
        "--input-bundle",
        type=Path,
        default=None,
        help="alias for the positional bundle path",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=None,
        help="output directory for the gtopt planning",
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
        help="show a summary of the bundle and exit",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="run schema sanity checks and exit (0 = ok)",
    )
    parser.add_argument(
        "--use-single-bus",
        action="store_true",
        help="collapse the multi-bus topology to a single bus (copperplate)",
    )
    parser.add_argument(
        "--default-uc-penalty",
        type=float,
        default=None,
        help=(
            "fallback penalty ($/unit-of-RHS) to apply to every "
            "user-constraint whose PLEXOS row carries no Penalty Price. "
            "Soft-cap diagnostic: keeps the LP feasible so the solver "
            "reports per-constraint violations instead of returning "
            "infeasible. Typical: 10000."
        ),
    )
    parser.add_argument(
        "--water-fail-cost",
        type=float,
        default=10.0,
        help=(
            "shared $/(m³/s·h) shortfall penalty applied uniformly to "
            "every soft hydro obligation emitted by the converter: "
            "soft FlowRights (Filt_Laja, Caudal_Eco_*, Riego_*, "
            "Ext_*), soft ``discharge_*min`` UserConstraints (turbine "
            "minimum-flow floors), and any future Flow slack on "
            "natural-inflow shortfall.  Default 10 matches plp2gtopt's "
            "``--water-fail-cost`` so PLEXOS- and PLP-derived JSONs "
            "use the same water-obligation pricing.  Higher values "
            "make the LP try harder to meet the soft target before "
            "violating; lower values let it skip the obligation more "
            "freely."
        ),
    )
    parser.add_argument(
        "--spill-fcost",
        type=float,
        default=None,
        help=(
            "override the per-flow spill cost ($/(m³/s·h)) on every "
            "``Vert_*`` spillway waterway.  When unset (default), the "
            "converter uses PLEXOS's ``Max Flow Penalty`` property "
            "(typically 3.6 on CEN PCP).  Set this to a larger value "
            "(e.g. 1000) to discourage the LP from routing surplus "
            "water through Vert_* arcs, pushing it toward turbines / "
            "bypasses / other paths instead.  Useful for tuning the "
            "turbine-vs-spill tradeoff when comparing against PLEXOS."
        ),
    )
    parser.add_argument(
        "--use-plexos-commit",
        action="store_true",
        default=False,
        help=(
            "override per-period generator pmax with PLEXOS-solved "
            "Units Generating (pid 7) from the solution .accdb cache. "
            "Pins gen=0 at hours PLEXOS left the unit OFF.  Used to "
            "validate that gtopt's hydro over-dispatch is driven by "
            "the missing MIP commitment decisions: applying this "
            "forces dispatch into the PLEXOS-chosen ON windows only, "
            "and downstream cascade hydro should snap to PLEXOS's "
            "observed values."
        ),
    )
    parser.add_argument(
        "--reservoir-spillway",
        action="store_true",
        default=False,
        help=(
            "activate Reservoir-internal spillway with cost=0 on every "
            "reservoir, and disable the Vert_→Junction.drain collapse. "
            "Mirrors PLEXOS's implicit Storage-state spillage."
        ),
    )
    parser.add_argument(
        "--use-plexos-gen-cap",
        action="store_true",
        default=False,
        help=(
            "hard-cap per-period generator pmax to PLEXOS-solved "
            "Generation (pid 2) from the solution .accdb cache. "
            "TIGHTEST curve-fit: every block, gtopt's LP can dispatch "
            "at most what PLEXOS dispatched in that block.  Useful for "
            "validating that gtopt's overshoot is purely from missing "
            "per-block dispatch envelopes vs structural differences. "
            "Restricted to HYDRO TURBINE generators to avoid the "
            "ReserveProvisionLP::flat_map::at defect at zero-pmax "
            "blocks on thermal units."
        ),
    )
    parser.add_argument(
        "--vert-routing",
        choices=("ocean", "cascade"),
        default="ocean",
        help=(
            "destination of every ``Vert_*`` spillway waterway. "
            "DIAGNOSTIC TOGGLE — exists solely to reproduce a "
            "strange PLEXOS behaviour (mid-cascade Vert spillage) "
            "that is not natural to our LP; not a physical "
            "modelling choice.  ``ocean`` (default): every Vert_* "
            "→ <source>_ocean drain; spillage LEAVES the topology. "
            "``cascade``: keep PLEXOS's published Tail Storage "
            "target so spillage feeds the next cascade junction "
            "(can be re-turbined downstream); falls back to ocean "
            "when PLEXOS publishes no downstream target.  Default "
            "is whichever produces dispatch closer to PLEXOS on "
            "the reference bundle (currently ``ocean``)."
        ),
    )
    parser.add_argument(
        "--spill-fcost-scale",
        type=float,
        default=1.0,
        help=(
            "multiplicative scale on PLEXOS's ``Max Flow Penalty`` for "
            "every ``Vert_*`` spillway waterway.  Default 1.0 (no "
            "rescaling).  Applied AFTER ``--spill-fcost`` (if set), so "
            "the two are composable.  Useful when the relative cost "
            "structure is right but the absolute level is wrong."
        ),
    )
    parser.add_argument(
        "--horizon-mode",
        choices=("plexos", "hourly"),
        default="plexos",
        help=(
            "block-layout mode.  ``plexos`` (default) reproduces "
            "PLEXOS's exact block distribution from the solution "
            "``.accdb`` (``t_phase_3`` table) — for CEN PCP daily "
            "that's 111 blocks over 7 days with [24, 20, 13, 14, 12, "
            "15, 13] per day.  Falls back to uniform daily blocks "
            "from ``PLEXOS_Param.xml`` band counts when no .accdb is "
            "available.  ``hourly`` emits ``--horizon-days × 24`` "
            "uniform hourly blocks (168 for a full week) with no "
            "aggregation."
        ),
    )
    parser.add_argument(
        "--horizon-days",
        type=int,
        default=None,
        choices=range(1, 8),
        metavar="N",
        help=(
            "number of consecutive days to convert (1-7).  In "
            "``--horizon-mode hourly`` defaults to 1 (legacy "
            "behaviour).  In ``--horizon-mode plexos`` the day count "
            "is derived from the PLEXOS solution and this flag is "
            "ignored."
        ),
    )
    parser.add_argument(
        "--plexos-solution-accdb",
        type=Path,
        default=None,
        help=(
            "path to the PLEXOS solution ``.accdb`` (used to extract "
            "the block layout under ``--horizon-mode plexos``).  When "
            "omitted, the converter auto-discovers a sibling "
            "``RES<date>.zip[.xz]`` next to the input bundle and "
            "extracts the nested .accdb from it."
        ),
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def _resolve_bundle(args: argparse.Namespace) -> Path | None:
    """Pick ``-i`` / ``--input-bundle`` over the positional argument."""
    if args.input_bundle is not None:
        return args.input_bundle
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

    bundle = _resolve_bundle(args)
    if bundle is None:
        parser.error(
            "input bundle is required: pass it positionally or via --input-bundle"
        )

    options = {
        "input_bundle": bundle,
        "output_dir": args.output_dir,
        "use_single_bus": args.use_single_bus,
        "default_uc_penalty": args.default_uc_penalty,
        "water_fail_cost": args.water_fail_cost,
        "spill_fcost": args.spill_fcost,
        "spill_fcost_scale": args.spill_fcost_scale,
        "vert_routing": args.vert_routing,
        "use_plexos_commit": args.use_plexos_commit,
        "use_plexos_gen_cap": args.use_plexos_gen_cap,
        "reservoir_spillway": args.reservoir_spillway,
        "horizon_mode": args.horizon_mode,
        "horizon_days": args.horizon_days,
        "plexos_solution_accdb": args.plexos_solution_accdb,
    }

    if args.show_info:
        try:
            display_plexos_info(options)
        except (FileNotFoundError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            sys.exit(1)
        return

    if args.validate:
        sys.exit(0 if validate_plexos_bundle(options) else 1)

    # Default action: convert. Returns 0 on success, or the count of
    # CRITICAL findings the converter logged (so CI can gate on it).
    try:
        critical = convert_plexos_bundle(options) or 0
    except NotImplementedError as exc:
        print(f"plexos2gtopt: {exc}", file=sys.stderr)
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
