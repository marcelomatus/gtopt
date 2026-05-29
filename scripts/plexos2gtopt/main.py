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
from .parsers import UnresolvedConstraintReferenceError
from .plexos2gtopt import (
    compare_plexos_bundle,
    convert_plexos_bundle,
    validate_plexos_bundle,
)


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
        "--compare",
        dest="compare_json",
        default=None,
        metavar="GTOPT_JSON",
        help=(
            "standalone re-comparison: parse the PLEXOS bundle and compare it "
            "against an already-converted gtopt planning JSON (element counts, "
            "user-constraint families, global indicators), then exit. No "
            "conversion is performed."
        ),
    )
    parser.add_argument(
        "--no-check",
        dest="run_check",
        action="store_false",
        help=(
            "skip the post-conversion PLEXOS↔gtopt comparison and structural "
            "validation (on by default, mirroring plp2gtopt)"
        ),
    )
    parser.add_argument(
        "--lax-uc-refs",
        action="store_true",
        help=(
            "downgrade the strict UserConstraint-reference check from a "
            "fail-hard error to a warning, silently dropping the offending "
            "terms.  Use for DEBUGGING / iterative parser work when dangling "
            "LHS refs (Vert_*, circuit-suffix lines, _SC shadows) block "
            "regeneration; the resulting JSON is internally consistent but "
            "the rescued constraints may be weakened.  Pair with "
            "`gtopt --constraint-mode debug` for the matching gtopt-side "
            "leniency."
        ),
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
        "--pampl-uc-mode",
        choices=("hard", "soft", "off"),
        default="hard",
        help=(
            "how to emit the per-family user-constraint .pampl files: "
            "'hard' (default) keeps each PLEXOS row's own penalty (rows "
            "without one stay hard); 'soft' forces a slack penalty on EVERY "
            "row (uses --default-uc-penalty, or 10000 if unset) so the MIP "
            "stays feasible and reports violations instead of going "
            "infeasible; 'off' drops the user constraints entirely (no "
            ".pampl files, no inline rows). Diagnostic: the hard PLEXOS UCs "
            "can over-constrain integer commitment into infeasibility."
        ),
    )
    parser.add_argument(
        "--pampl-uc-only",
        default=None,
        metavar="FAM[,FAM...]",
        help=(
            "emit ONLY these user-constraint families (comma-separated), "
            "dropping all others.  Diagnostic for isolating which group "
            "over-constrains the MIP — e.g. --pampl-uc-only config_exclusivity. "
            "Families: config_exclusivity, gas_offtake, commitment, reserve, "
            "security, comparison, terminal_value, operational."
        ),
    )
    parser.add_argument(
        "--pampl-uc-off",
        default=None,
        metavar="FAM[,FAM...]",
        help=(
            "drop these user-constraint families (comma-separated), keeping "
            "the rest (leave-one-out diagnostic).  Same family names as "
            "--pampl-uc-only."
        ),
    )
    parser.add_argument(
        "--fcf-scale-alpha",
        type=float,
        default=None,
        help=(
            "column scale for the FCF cost-to-go variable alpha_fcf (default "
            "1e6).  The cut/objective coefficient on alpha is this value, so "
            "alpha carries future_cost/scale_alpha; pick it near the "
            "cost-to-go magnitude (~1e6) to land the alpha column at O(1-1e3) "
            "like the other LP variables.  Tune (1 / 1e3 / 1e6 …) to study LP "
            "conditioning; independent of the alpha-rebase shift."
        ),
    )
    parser.add_argument(
        "--uc-emit",
        choices=("pampl", "inline"),
        default="pampl",
        help=(
            "where to emit user constraints: 'pampl' (default) writes the "
            "modular per-family .pampl files (system.user_constraint_files); "
            "'inline' keeps them in system.user_constraint_array in the JSON "
            "(the legacy path).  Diagnostic: generate the case both ways and "
            "diff the LPs to check whether gtopt's PAMPL parser and JSON "
            "parser produce the same matrix."
        ),
    )
    parser.add_argument(
        "--soft-penalty-cost",
        type=float,
        default=None,
        help=(
            "explicit $/MWh override for the shared soft-penalty used by "
            "BOTH the forced-`pmin` floor (Generator.pmin_fcost on "
            "non-renewable Fixed Load units) AND the soft line-overload / "
            "EL=1 slack cost.  When unset (default) the converter computes "
            "min(max(gcost)+1, min(VoLL)-1) from the bundle's own cost "
            "data: high enough to honour forced dispatch / re-dispatch, "
            "capped below the cheapest unserved-demand cost so load-"
            "serving always outranks it."
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
        choices=("uniform", "equal_error", "tangent", "midpoint"),
        default="midpoint",
        help=(
            "Segment-layout strategy for the PWL line-loss approximation, "
            "emitted on every lossy ``Line.loss_pwl_layout`` entry.  "
            "``midpoint`` (DEFAULT): de-biased secant — the chord with the "
            "uniform secant slope shifted DOWN to the mid-point tangent so "
            "the PWL is unbiased (exact at segment midpoints) instead of an "
            "upper bound.  On the CEN PCP daily case (with envelope "
            "decoupling auto-emitted on softened lines) K=4 midpoint matches "
            "PLEXOS losses to -1.6%% and operational cost to +0.6%%, vs the old "
            "uniform secant's +41%% loss / +5%% cost — segment count barely "
            "matters once de-biased.  ``uniform``: equal-width secant chords "
            "(strict UPPER bound; max chord error peaks on the outer "
            "segment — overstates losses).  ``tangent``: outer-approximation "
            "tangents (lower bound, exact at touch points; inequality rows "
            "resist presolve binary-fixing — heavier MIP).  ``equal_error``: "
            "√-spaced secant chords (aliases to uniform for the convex "
            "quadratic — see line_losses.cpp seg_geom docstring)."
        ),
    )
    parser.add_argument(
        "--emin-eod-day1",
        dest="emin_eod_day1",
        action="store_true",
        default=False,
        help=(
            "enforce the PLEXOS operational reservoir floor from "
            "``Hydro_MinVolume.csv`` at the end of day 1 (hour 24) as "
            "a HARD per-block ``emin`` clamp.  Recovers the daily-"
            "coordination signal PLEXOS uses to keep reservoirs above "
            "their published operational floor at midnight of day 1.  "
            "Default OFF.  Pass ``--emin-eod-day1`` to enable.  This "
            "hard hour-24 clamp pins reservoirs near full at midnight of "
            "day 1, blocking day-1 drawdown and inflating reservoir "
            "water-value duals well above PLEXOS (e.g. CANUTILLAR 2.6×, "
            "ELTORO/L_Maule ~1.1-1.2×); disabling it brings the duals "
            "back in line with PLEXOS's storage Shadow Price.  End-of-"
            "week (last day) floors are honoured separately as a soft "
            "``efin`` + ``efin_cost`` slack.  Mid-week (hour 48, 72, …) "
            "floors stay disabled to avoid the L_Maule infeasibility "
            "chain that prompted removing per-block clamps in the first "
            "place."
        ),
    )
    parser.add_argument(
        "--no-emin-eod-day1",
        dest="emin_eod_day1",
        action="store_false",
        help="disable the hour-24 hard emin floor (now the default; "
        "see --emin-eod-day1).",
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
        default=None,
        help=(
            "Piecewise-linear loss-segment count.  Interpretation depends "
            "on ``--loss-error-pct``:\n"
            "  * Adaptive mode (default, ``--loss-error-pct > 0``): "
            "``--nseg-losses`` is the CEILING on the per-line K computed "
            "by the cube-root rule (``K_i ∝ L_max,i^(1/3)``).  When not "
            "set, ceiling defaults to **6**.\n"
            "  * Uniform mode (``--loss-error-pct 0``): every lossy line "
            "gets exactly ``--nseg-losses`` segments.  When not set, "
            "uniform K defaults to **4** (the historic CEN PCP value).\n"
            "Floor is always 2 (a single secant is degenerate).  "
            "L_max,i = R·fmax² (peak loss MW); global scale c = √(S/(4·B)) "
            "with S = Σ L^(1/3) and B = ``--loss-error-pct × Σ L_max``.  "
            "Worst-case per-segment error scales as 1/K² under the "
            "``midpoint`` de-bias."
        ),
    )
    parser.add_argument(
        "--loss-extend-overload",
        action="store_true",
        default=False,
        help=(
            "EXPERIMENTAL — DEFAULT OFF.  Extend each soft-cap line's PWL "
            "``loss_envelope`` from ``[0, tmax_normal]`` to "
            "``[0, headroom_factor × tmax_normal]`` (= the full lifted "
            "tmax_ab) so the segments cover the overload band the LP can "
            "actually flow into.  Adaptive K (``--loss-error-pct``) sizes "
            "K_i for the extended envelope so the worst-case secant-error "
            "budget is honoured across the wider range.\n"
            "\n"
            "When ON:\n"
            "  * EL=1 / EL=2 (hard cap, LP cannot exceed): unchanged.\n"
            "  * EL=0 soft-cap (regular):       envelope → 2 × tmax_normal.\n"
            "  * --lift-line-caps (wider band): envelope → 4 × tmax_normal.\n"
            "\n"
            "WHY THE DEFAULT IS OFF — the trade-off in three flow regimes:\n"
            "\n"
            "  Regime A (f ∈ [0, tmax_normal]):  the same K segments are\n"
            "    now spread over 2× the range, so per-segment width is\n"
            "    2× wider and worst-case secant error grows as width² →\n"
            "    **4× worse** at any flow in the normal operating band.\n"
            "    Adaptive K partly compensates (K_i grows ~1.59× since\n"
            "    L_max grows 4× and K_i ∝ L_max^(1/3)), but the ceiling=6\n"
            "    clamp usually binds on heavy lines, so most of the 4×\n"
            "    degradation survives in practice.\n"
            "\n"
            "  Regime B (f ∈ (tmax_normal, (1 + 1/K) · tmax_normal)):  OFF's\n"
            "    linear extrapolation of the last segment's slope still\n"
            "    beats ON's wider-segment secant approximation.  Crossover\n"
            "    is at f = tmax_normal · (1 + 1/K):\n"
            "          K = 2  → f_cross = 1.50 × tmax_normal\n"
            "          K = 3  → f_cross = 1.33 × tmax_normal\n"
            "          K = 4  → f_cross = 1.25 × tmax_normal\n"
            "          K = 6  → f_cross = 1.17 × tmax_normal\n"
            "          K = 8  → f_cross = 1.13 × tmax_normal\n"
            "\n"
            "  Regime C (f > (1 + 1/K) · tmax_normal):  OFF's extrapolation\n"
            "    error grows quadratically as (f − tmax_normal)², so ON\n"
            "    eventually wins.  At f = 2·tmax_normal (the lifted cap),\n"
            "    OFF underestimates loss by ~25 % of the true value while\n"
            "    ON stays bounded by the same per-segment budget as below.\n"
            "\n"
            "For healthy systems with slack capacity, the LP almost never\n"
            "leaves Regime A.  Measured on the CEN PCP weekly LP-relax\n"
            "(K=4 uniform and adaptive @ 1 % / 5 % / 10 %):\n"
            "  * 0 of 175 soft-cap lines  ever cross tmax_normal\n"
            "  * max line utilization      = 80 % of tmax_normal\n"
            "  * → every operating point is in Regime A where OFF is\n"
            "    strictly better.\n"
            "Turning ON in that scenario gives 4× worse PWL fidelity in\n"
            "[0, tmax_normal] for **zero offsetting benefit** anywhere the\n"
            "LP actually operates.\n"
            "\n"
            "HEURISTIC: leave OFF for routine production runs.  Flip ON\n"
            "only when you expect non-trivial flow past ~(1 + 1/K)·tmax_normal\n"
            "— N-1 contingency MIPs, peak-stress dispatches, lifted-cap\n"
            "scenarios with tight reserves, or when a post-solve audit shows\n"
            "non-trivial overload-band traffic on multiple lines."
        ),
    )
    parser.add_argument(
        "--loss-error-pct",
        type=float,
        default=0.01,
        help=(
            "Target absolute system-loss-error budget as a fraction of "
            "Σ L_max,i (the sum of per-line peak losses ``R·fmax²``).  "
            "Default **0.01** (1%%); the adaptive cube-root rule then "
            "allocates per-line ``K_i ∝ L_max,i^(1/3)`` so the LP-side "
            "PWL approximation has bounded total worst-case error in "
            "MW.  Pass ``0`` to disable adaptation and force the legacy "
            "uniform-K behaviour (every line gets ``--nseg-losses`` "
            "segments).  Higher values (e.g. ``0.02``) shrink the LP "
            "faster but accept more loss error; lower values (e.g. "
            "``0.005``) tighten accuracy at the cost of more LP "
            "non-zeros.  The per-line K stays clamped to "
            "[2, ``--nseg-losses``] regardless of budget."
        ),
    )
    # NOTE: ``--loss-tangent-top-pct`` (the loading-classified R·P²
    # percentile RANKING that put the top-P% lossiest lines on the tangent
    # layout) was REMOVED.  The ``midpoint`` de-bias + per-line
    # ``loss_envelope`` decoupling match PLEXOS losses to ~2% at K=4 without
    # the MIP-heavy hybrid tangent tier, so the ranking is obsolete.  The
    # explicit ``--loss-tangent-lines`` escape hatch remains below.
    parser.add_argument(
        "--loss-tangent-lines",
        type=str,
        default=None,
        metavar="NAME[,NAME...]",
        help=(
            "Comma-separated line names forced to the ``tangent`` loss "
            "layout (with ``--nseg-tangent`` segments); all other lines use "
            "the ``--loss-pwl-layout`` base layout.  Useful for known "
            "binding interconnections."
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
        default=4,
        help=(
            "Segment count for the base (``uniform``/``midpoint``) layout "
            "lines — i.e. ALL lines except those in ``--loss-tangent-lines`` "
            "(the default).  DEFAULT 4: with the ``midpoint`` de-bias + "
            "envelope decoupling, K=4 already matches PLEXOS losses to "
            "~-1.6%% (the loss-accuracy sweet spot); more segments drift "
            "slightly under and add LP rows for no benefit.  These layouts "
            "use segment-variable rows that presolve handles cheaply."
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
        "--el0-lines",
        choices=("extended", "strict"),
        default="extended",
        help=(
            "How to model PLEXOS EL=0 ('Never enforce') transmission lines:\n"
            "  'extended' (default, a.k.a. relaxed): soft cap — flow is free "
            "up to the rating, penalised between the rating and a headroom "
            "multiple, hard-capped at that multiple.  Mirrors PLEXOS, which "
            "leaves these lines uncapped yet runs the radial pockets above "
            "rating; stops the DC-OPF from teleporting GWs across them.\n"
            "  'strict': treat EL=0 like EL=2 — a plain hard cap at the "
            "nominal rating (no free band, no headroom).  Use when you want "
            "every line rating enforced.  (Lines named in --lift-line-caps "
            "stay soft regardless.)"
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
        "--use-plexos-efin",
        action="store_true",
        default=False,
        help=(
            "DEBUG: pin Reservoir.efin to the PLEXOS-SOLVED End Volume "
            "(prop 646, last period) from the solution .accdb cache, "
            "curve-fitting gtopt's terminal storage to PLEXOS's answer. "
            "DEFAULT (flag off) is fully input-driven: the end-of-horizon "
            "target is the last-day floor from Hydro_MinVolume.csv (the "
            "operational target PLEXOS encodes by raising the min-volume "
            "floor at the final period)."
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
        "--block-layout",
        default=None,
        metavar="CSV|SPEC",
        help=(
            "user-defined block layout for ``--horizon-mode plexos`` when "
            "no PLEXOS solution .accdb is available (the 111-block grouping "
            "lives only in the solution).  Accepts EITHER a path to a CSV "
            "with a 'duration' column (one row per block, optional "
            "'block_uid' column for order), OR an inline string: "
            "'{1:1,2:4,3:2}' (block_uid:duration) or '1,4,2' (durations in "
            "block order).  duration = consecutive 1-hour intervals per "
            "block; the converter builds the chronological grouping and "
            "sets the horizon from the total.  Falls back to uniform hourly "
            "when neither a solution nor this flag is provided."
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
        "pampl_uc_mode": args.pampl_uc_mode,
        "pampl_uc_only": args.pampl_uc_only,
        "pampl_uc_off": args.pampl_uc_off,
        "uc_emit": args.uc_emit,
        "fcf_scale_alpha": args.fcf_scale_alpha,
        "soft_penalty_cost": args.soft_penalty_cost,
        "water_fail_cost": args.water_fail_cost,
        "spill_fcost": args.spill_fcost,
        "spill_fcost_scale": args.spill_fcost_scale,
        "vert_routing": args.vert_routing,
        "use_plexos_commit": args.use_plexos_commit,
        "use_plexos_gen_cap": args.use_plexos_gen_cap,
        "use_plexos_efin": args.use_plexos_efin,
        "nseg_losses": args.nseg_losses,
        "loss_error_pct": args.loss_error_pct,
        "loss_extend_overload": args.loss_extend_overload,
        "loss_pwl_layout": args.loss_pwl_layout,
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
        "el0_lines": args.el0_lines,
        "horizon_mode": args.horizon_mode,
        "horizon_days": args.horizon_days,
        "block_layout": args.block_layout,
        "plexos_solution_accdb": args.plexos_solution_accdb,
        "lp_relax": args.lp_relax,
        "run_check": args.run_check,
        "compare_json": args.compare_json,
        "lax_uc_refs": args.lax_uc_refs,
    }

    if args.compare_json:
        # Standalone re-comparison against an existing gtopt JSON; no convert.
        try:
            critical = compare_plexos_bundle(options) or 0
        except (FileNotFoundError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            sys.exit(1)
        sys.exit(1 if critical > 0 else 0)

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
    except UnresolvedConstraintReferenceError as exc:
        # A UserConstraint term referenced an element gtopt never emits.
        # Fail loudly with the FULL list — never write a bundle that
        # carries dangling references (mirrors gtopt's strict parser).
        print(
            f"error: unresolved UserConstraint reference(s):\n{exc}",
            file=sys.stderr,
        )
        sys.exit(1)
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
