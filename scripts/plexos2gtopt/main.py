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

from .auto_lift_lines import DEFAULT_THRESHOLD
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
        "--loss-pwl-layout",
        choices=("uniform", "equal_error", "tangent"),
        default="tangent",
        help=(
            "Segment-layout strategy for the PWL line-loss approximation, "
            "emitted on every lossy ``Line.loss_pwl_layout`` entry.  "
            "``tangent`` (default since CEN PCP K=3/K=4 sweeps showed "
            "best obj and closest match to PLEXOS losses; outer-"
            "approximation tangents at uniform midpoints — bounds loss "
            "BELOW the true quadratic curve, exact at the touching point).  "
            "``uniform``: equal-width secant chords; max chord error "
            "peaks on the outer segment.  ``equal_error``: √-spaced "
            "secant chords (minimax) — same K, same LP row count, "
            "max chord error drops by ~√K (currently aliases to "
            "uniform — see line_losses.cpp seg_geom docstring)."
        ),
    )
    parser.add_argument(
        "--emin-eod-day1",
        dest="emin_eod_day1",
        action="store_true",
        default=True,
        help=(
            "enforce the PLEXOS operational reservoir floor from "
            "``Hydro_MinVolume.csv`` at the end of day 1 (hour 24) as "
            "a HARD per-block ``emin`` clamp.  Recovers the daily-"
            "coordination signal PLEXOS uses to keep reservoirs above "
            "their published operational floor at midnight of day 1.  "
            "Default ON.  Pass ``--no-emin-eod-day1`` to disable.  "
            "End-of-week (last day) floors are honoured separately as "
            "a soft ``efin`` + ``efin_cost`` slack.  Mid-week (hour 48, "
            "72, …) floors stay disabled to avoid the L_Maule "
            "infeasibility chain that prompted removing per-block "
            "clamps in the first place."
        ),
    )
    parser.add_argument(
        "--no-emin-eod-day1",
        dest="emin_eod_day1",
        action="store_false",
        help="disable the hour-24 hard emin floor (see --emin-eod-day1).",
    )
    parser.add_argument(
        "--battery-efin-pin",
        dest="battery_efin_pin",
        action="store_true",
        default=True,
        help=(
            "pin every battery's end-of-horizon SoC to its initial "
            "SoC (``efin = eini``).  Default ON — forces the LP to "
            "return batteries to their starting state, preventing "
            "off-spec end-of-horizon energy banking that drives "
            "BESS net-charge by ~12 GWh on the CEN PCP weekly "
            "bundle.  Pass ``--no-battery-efin-pin`` to drop the "
            "pin and let the LP set ``efin`` freely (matches "
            "PLEXOS's flexible terminal-SoC convention; on CEN PCP "
            "weekly 2026-04-22 PLEXOS net-discharges batteries by "
            "≈568 MWh over the horizon)."
        ),
    )
    parser.add_argument(
        "--no-battery-efin-pin",
        dest="battery_efin_pin",
        action="store_false",
        help="drop the efin=eini pin (see --battery-efin-pin).",
    )
    parser.add_argument(
        "--soft-efin-reservoirs",
        type=str,
        default="L_Maule",
        help=(
            "Comma-separated reservoir names whose end-of-horizon "
            "``efin`` is emitted as a SOFT constraint (slack column at "
            "the reservoir's PLEXOS Water Value, or $1e6/GWh for the "
            "PLEXOS ``1e+30`` never-drain sentinel reservoirs).  All "
            "other reservoirs get a HARD ``vol_end >= efin`` row.  "
            "Default ``L_Maule`` — its 1e+30 Water Value makes the "
            "floor unreachable without slack once the upstream cascade "
            "is tightened; every other CEN PCP reservoir reaches its "
            "PLEXOS-published efin natively.  Pass an empty string "
            "(``--soft-efin-reservoirs=''``) to make EVERY efin hard, "
            "or a comma-separated list to opt in additional reservoirs."
        ),
    )
    parser.add_argument(
        "--nseg-losses",
        type=int,
        default=4,
        help=(
            "number of piecewise-linear segments used to approximate "
            "the quadratic transmission-loss curve P_loss = R·f²/V² on "
            "each lossy line (PLEXOS Enforce Limits = 2 lines with a "
            "non-zero resistance and a finite ``tmax_ab`` envelope).  "
            "Default 4 with the ``tangent`` layout — chosen from CEN "
            "PCP sweeps as the best obj + closest match to PLEXOS "
            "losses (0 MWh unserved, +1.63%% loss vs PLEXOS).  Larger "
            "values (6, 10) reduce the PWL approximation error at the "
            "cost of more LP rows / variables.  PWL error at f = f_max "
            "scales as 1/nseg, so nseg=6 halves the worst-case loss "
            "overestimate on the outer segment."
        ),
    )
    parser.add_argument(
        "--loss-tangent-top-pct",
        type=float,
        default=30.0,
        metavar="PCT",
        help=(
            "Enable the loading-classified HYBRID loss layout: the top "
            "PCT%% of lossy lines BY STATIC LOSS MAGNITUDE R·P² "
            "(resistance × rating², the V²-free part of the full-flow "
            "loss (R/V²)·P²) get the accurate ``tangent`` layout with "
            "``--nseg-tangent`` segments; the remaining lossy lines get "
            "the cheaper, presolve-friendly ``uniform`` layout with "
            "``--nseg-uniform`` segments.  PCT=0 → all uniform; PCT=100 "
            "→ all tangent; PCT=20 → the 20%% highest-loss lossy lines "
            "are tangent.  DEFAULT 30 (covers ~50%% of the realised "
            "losses on CEN PCP while keeping ~70%% of lines on the fast "
            "uniform layout — best accuracy/MIP-size trade-off found).  "
            "R·P² ranks far better than rating alone (a "
            "high-rating low-R trunk carries little loss).  Spends the "
            "MIP-heavy tangent rows only where loss concentrates (Sun et "
            "al. 2019, line-loading classification).  Overrides "
            "``--loss-pwl-layout`` / ``--nseg-losses`` when set.  Combine "
            "with ``--loss-tangent-lines`` to additionally force specific "
            "lines to tangent regardless of loss."
        ),
    )
    parser.add_argument(
        "--loss-tangent-lines",
        type=str,
        default=None,
        metavar="NAME[,NAME...]",
        help=(
            "Comma-separated line names forced to the ``tangent`` loss "
            "layout regardless of rating (also activates hybrid mode on "
            "its own).  Useful for known binding interconnections."
        ),
    )
    parser.add_argument(
        "--nseg-tangent",
        type=int,
        default=6,
        help=(
            "Segment count for ``tangent``-layout lines in hybrid mode "
            "(default 6).  Tangent uses inequality (outer-approximation) "
            "rows that resist presolve binary-fixing, so keep it coarse."
        ),
    )
    parser.add_argument(
        "--nseg-uniform",
        type=int,
        default=8,
        help=(
            "Segment count for ``uniform``-layout lines in hybrid mode "
            "(default 8).  Uniform uses segment-variable equalities that "
            "presolve handles cheaply, so it can afford more segments."
        ),
    )
    parser.add_argument(
        "--auto-lift-lines",
        nargs="?",
        type=float,
        const=DEFAULT_THRESHOLD,
        default=None,
        metavar="THRESHOLD",
        help=(
            "After writing the bundle JSON, run a lifted-cap DC OPF on "
            "the first (scenario, block) and post-patch "
            "``enforce_level = 0`` onto every line whose lifted-OPF "
            "flow exceeds its rated ``tmax_ab`` by at least THRESHOLD "
            "(default 1.0 = strictly above rated).  OPF engine selected "
            "via ``--auto-lift-engine``.  Mirrors PLEXOS's voltage-"
            "conditional cap treatment on radial step-down lines (e.g. "
            "``Capricornio110->LaNegra110`` on CEN PCP, 76 MW rated, "
            "~204 MW PLEXOS dispatch).  Composes with "
            "``--lift-line-caps``: the auto-detected set is the UNION "
            "with the comma-separated names there.  Falls back to a "
            "logged warning + empty patch when the engine is "
            "unavailable.  Pass without a value to use the default "
            "threshold."
        ),
    )
    parser.add_argument(
        "--auto-lift-engine",
        choices=("pandapower", "gtopt"),
        default="pandapower",
        help=(
            "OPF engine for ``--auto-lift-lines``.  ``pandapower`` "
            "(default): runs ``pp.rundcopp`` on the first block via "
            "``gtopt2pp.convert``; ~10 s on CEN PCP weekly; needs the "
            "``pandapower`` + ``gtopt2pp`` Python packages.  ``gtopt``: "
            "runs the actual ``gtopt --no-mip`` binary on a sibling "
            "bundle with every line demoted to ``enforce_level = 0``, "
            "then reads the first-block flows from the parquet output; "
            "~30 s on CEN PCP weekly; needs only the ``gtopt`` binary "
            "in ``$PATH`` (or via ``$GTOPT_BIN``).  Use ``gtopt`` when "
            "you want the EXACT loss model + voltage assumptions the "
            "downstream solve will see, ``pandapower`` for speed."
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
        "--lift-line-caps",
        type=str,
        default="Capricornio110->LaNegra110",
        help=(
            "Comma-separated list of Line names to demote from PLEXOS "
            "EL=1 (enforce hard cap) down to EL=0 (no cap, but keep "
            "tmax_ab for loss-segment discretization).  Used for "
            "PLEXOS lines where the dispatched flow exceeds the "
            "published rating because the line is radial and the LP "
            "has no alternative path — enforcing the cap in gtopt "
            "would otherwise create unserved demand.\n"
            "\n"
            "Default lifts ``Capricornio110->LaNegra110`` only — the "
            "single canonical case on the CEN PCP weekly bundle (76 "
            "MW Max Flow, 204 MW in PLEXOS dispatch, 269%% of cap; "
            "the line is a 110 kV radial stepdown to the Antofagasta "
            "region with no parallel path).  Pass an empty string "
            "(``--lift-line-caps=''``) to activate the experimental "
            "SOFT-EL=1 mode instead — every EL=1 line gets a parallel "
            "slack at ``tcost = (min(demand.fcost) + max(generator."
            "gcost)) / 2`` ($/MWh).  Soft mode lets the LP push past "
            "the PLEXOS rating at a penalty, but on CEN PCP weekly "
            "increased BESS-charging +77%% and losses +18%% vs the "
            "Capricornio-only baseline — kept as an opt-in for new "
            "bundles where the lift list isn't curated yet."
        ),
    )
    parser.add_argument(
        "--reservoir-spillway",
        nargs="?",
        const="basic",
        default=None,
        choices=("basic", "strict"),
        help=(
            "activate Reservoir-internal spillway with cost=0 on every real "
            "reservoir.  Two modes:\n"
            "  basic (default when flag is bare): ONLY emit "
            "Reservoir.spillway_cost=0.  All other spillway mechanisms "
            "(Junction.drain, Vert_* waterways, fmax=∞+fcost>0 arcs) stay "
            "active — the LP picks whichever is cheapest, typically the "
            "free reservoir spillway.\n"
            "  strict: emit Reservoir.spillway_cost=0 AND disable every "
            "DUPLICATE spillway mechanism on real reservoirs — removes "
            "Junction.drain on real-reservoir junctions, drops any "
            "Vert_<X> waterway that survived the collapse, and clears "
            "fcost on pure-spillway waterways (fmax=∞ + fcost>0) that "
            "originate at a real reservoir.  Use this to FORCE all "
            "spillage through the reservoir's own internal drain "
            "(matches PLEXOS's hidden Storage-state spillage exactly)."
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
        "--lp-relax",
        action="store_true",
        default=False,
        help=(
            "emit ``Commitment.relax = true`` on every commitment so "
            "gtopt LP-relaxes the binary status / startup / shutdown "
            "variables.  Default (since 2026-05-23) is MIP — commitments "
            "ship without ``relax`` so gtopt enforces integrality.  "
            "Empirically: dropping the LP-relax closed ~7 pp of the "
            "PLEXOS-vs-gtopt dispatch-cost gap on CEN PCP, moving "
            "NUEVA_RENCA-TG+TV_GN_A from 19 GWh/week (LP) to 33 GWh/week "
            "(MIP, vs PLEXOS 40 GWh).  Pass ``--lp-relax`` for the "
            "legacy LP-only behaviour (faster solve, looser dispatch) "
            "or for solvers without MIP support."
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
        "nseg_losses": args.nseg_losses,
        "loss_pwl_layout": args.loss_pwl_layout,
        "loss_tangent_top_pct": args.loss_tangent_top_pct,
        "loss_tangent_lines": args.loss_tangent_lines,
        "nseg_tangent": args.nseg_tangent,
        "nseg_uniform": args.nseg_uniform,
        "emin_eod_day1": args.emin_eod_day1,
        "battery_efin_pin": args.battery_efin_pin,
        "soft_efin_reservoirs": tuple(
            n.strip() for n in args.soft_efin_reservoirs.split(",") if n.strip()
        ),
        "auto_lift_lines": args.auto_lift_lines,
        "auto_lift_engine": args.auto_lift_engine,
        "reservoir_spillway": args.reservoir_spillway,
        "lift_line_caps": args.lift_line_caps,
        "horizon_mode": args.horizon_mode,
        "horizon_days": args.horizon_days,
        "plexos_solution_accdb": args.plexos_solution_accdb,
        "lp_relax": args.lp_relax,
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
