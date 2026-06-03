# SPDX-License-Identifier: BSD-3-Clause
"""Shared ``argparse`` flag registrars for gtopt converters.

Single source of truth for the canonical CLI flags every gtopt
converter (``plp2gtopt``, ``plexos2gtopt``, ``sddp2gtopt``,
``pp2gtopt``) needs to expose — so the per-converter ``make_parser``
code stays focused on converter-specific options and the flag
defaults / help text / choice lists never drift between tools.

Each ``add_*_argument(parser, *, default=…, dialect=…)`` helper
adds a single ``argparse`` argument to the supplied *parser* and
returns nothing.  ``default=`` accepts per-converter overrides;
``dialect=`` selects between known semantic variants (e.g. the
plp2gtopt ``--use-single-bus`` uses ``BooleanOptionalAction`` while
the plexos2gtopt one uses ``store_true``).

The grouped :func:`add_common_arguments` helper installs every
canonical flag at once with per-flag default overrides — convenient
when a new converter wants the entire baseline.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any, Final

__all__ = [
    "DEFAULT_WRITE_OUT",
    "LINE_LOSSES_MODE_CHOICES",
    "add_aperture_chunk_size_argument",
    "add_common_arguments",
    "add_demand_fail_cost_argument",
    "add_emissions_arguments",
    "add_lift_line_caps_argument",
    "add_line_losses_mode_argument",
    "add_loss_cost_eps_argument",
    "add_scale_objective_argument",
    "add_use_kirchhoff_argument",
    "add_use_single_bus_argument",
    "add_write_out_argument",
]


# ---------------------------------------------------------------------------
# Canonical default for ``options.write_out``
# ---------------------------------------------------------------------------
#
# Exactly the streams every downstream gtopt consumer needs by default —
# the union of:
#
#   * ``sol``           — primal column values (generation, line flow,
#                          load, fail, etc.)
#   * ``dual``          — row duals (Bus/balance_dual, Junction/balance_dual,
#                          Reservoir/water_value_dual, Battery/energy_dual,
#                          Reservoir/efin_dual, etc.)
#   * ``rc:Generator``  — Generator/generation_cost (reduced cost of the
#                          primary generation column — required by
#                          ``gtopt_marginal_units`` for the basic-vs-bound
#                          test that identifies the marginal unit)
#   * ``rc:Line``       — Line/overload{p,n}_cost / loss_cost — used by
#                          the loss-audit + congestion-attribution paths
#
# Narrower than ``"all"`` (which also writes ``extras`` — every dispatch
# variable for every element, plus per-element shadow prices) so the
# on-disk solution footprint stays lean by default.  Override with
# ``--write-out all`` for the full audit-grade output, or with
# ``--write-out sol`` for a primal-only run.
DEFAULT_WRITE_OUT: Final[str] = "sol,dual,rc:Generator,Line"


# ---------------------------------------------------------------------------
# Canonical line-losses-mode choices
# ---------------------------------------------------------------------------
#
# Mirrors the C++ ``gtopt::LineLossesMode`` enum in
# ``include/gtopt/line_enums.hpp``.  The 8 modes (in enum order) are:
#
#   0 none, 1 linear, 2 piecewise, 3 bidirectional, 4 adaptive,
#   5 dynamic, 6 piecewise_direct, 7 tangent_signed_flow
#
# The list is exported as a constant so converters can introspect or
# extend it (e.g. for `--help` group ordering) without duplicating
# the string literals.
LINE_LOSSES_MODE_CHOICES: Final[tuple[str, ...]] = (
    "none",
    "linear",
    "piecewise",
    "bidirectional",
    "adaptive",
    "dynamic",
    "piecewise_direct",
    "tangent_signed_flow",
)


# ---------------------------------------------------------------------------
# Emissions (gtopt_shared.emissions integration)
# ---------------------------------------------------------------------------


def add_emissions_arguments(
    parser: argparse.ArgumentParser,
    defaults: dict[str, Any] | None = None,
) -> None:
    """Register ``--emissions`` / ``--emissions-file`` / ``--emissions-report``.

    ``--emissions`` is a master switch (off by default).  When set, the
    converter loads per-fuel CO2 factors from ``--emissions-file``
    (default: the bundled IPCC-2006 defaults), injects them onto every
    Fuel element that lacks one, synthesizes the
    ``emission_array['co2']`` pollutant row, and writes the
    ``--emissions-report``.  Any factor the converter source already
    supplied (PLEXOS XML, plp2gtopt's ``--plexos-overlay``, project
    JSON merge) always wins over the defaults.

    See :mod:`gtopt_shared.emissions` for the fill-in semantics.

    Uses the ``defaults`` dict-of-overrides convention (rather than the
    keyword-only ``default=`` of the other helpers in this module) for
    backward compatibility with the pre-#507 call sites in
    :mod:`plp2gtopt._parsers` and :mod:`plexos2gtopt.main`.
    """
    d = defaults or {}
    parser.add_argument(
        "--emissions",
        dest="emissions",
        action="store_true",
        default=d.get("emissions", False),
        help=(
            "MASTER SWITCH for emission processing.  When set, the "
            "converter loads per-fuel CO2 factors from --emissions-file "
            "(default: bundled IPCC-2006 defaults), injects them onto "
            "every Fuel element that lacks one, synthesizes the "
            "emission_array['co2'] pollutant row, and writes the "
            "emissions report.  Source-supplied factors (PLEXOS XML, "
            "--plexos-overlay, project JSON merge) always win.  Off by "
            "default."
        ),
    )
    parser.add_argument(
        "--emissions-file",
        dest="emissions_file",
        type=Path,
        metavar="PATH",
        default=d.get("emissions_file", None),
        help=(
            "Custom emissions JSON file.  Only meaningful when "
            "--emissions is set.  Default: the bundled IPCC-2006 Vol 2 "
            "Ch 1 Table 1.4 file at "
            "gtopt_shared/data/ipcc_emission_factors.json."
        ),
    )
    parser.add_argument(
        "--emissions-report",
        dest="emissions_report",
        type=Path,
        metavar="FILE",
        default=d.get("emissions_report", None),
        help=(
            "Write a JSON report of the emissions fill-in (factor_added / "
            "factor_preserved / unknown_fuels / emission_array status) to "
            "FILE.  Only meaningful when --emissions is set.  Defaults to "
            "<output-dir>/plexos_emissions_report.json."
        ),
    )
    parser.add_argument(
        "--only-emissions",
        dest="only_emissions",
        action="store_true",
        default=d.get("only_emissions", False),
        help=(
            "Configure the LP for pure-emissions objective mode (issue "
            "#519).  Implies --emissions.  Emits "
            "``model_options.objective_mode = 'emissions'`` (gtopt swaps "
            "the LP objective from $-dispatch-cost to tCO2eq dispatch) "
            "AND stamps the synthesised EmissionZone with "
            "``price = 35.0 USD/tCO2eq`` (Chile's social cost of carbon, "
            "the Comisión Nacional de Energía reference for emission "
            "opportunity cost in least-cost dispatch).  In cost-mode "
            "runs the price is left unset, so dispatch is not silently "
            "distorted by a phantom carbon tax."
        ),
    )
    parser.add_argument(
        "--carbon-price",
        dest="carbon_price",
        type=float,
        metavar="USD_PER_TCO2EQ",
        default=d.get("carbon_price", None),
        help=(
            "Override the default carbon price stamped on EmissionZone "
            "when --only-emissions is set.  Default: 35.0 USD/tCO2eq "
            "(Chile SCC).  Ignored without --only-emissions."
        ),
    )
    parser.add_argument(
        "--emissions-discount-rate",
        dest="emissions_discount_rate",
        type=float,
        metavar="RATE",
        default=d.get("emissions_discount_rate", 0.05),
        help=(
            "Annual discount rate used to build the synthetic emissions "
            "ray (Benders cut equivalent) that replaces the cost-mode FCF "
            "in --only-emissions mode.  Only consumed by plp2gtopt's "
            "boundary-cut writer when --only-emissions is also set. "
            "Default: 0.05 (5%% / yr, CNE reference for hydro least-cost "
            "dispatch).  See issue #520."
        ),
    )
    parser.add_argument(
        "--emissions-horizon-years",
        dest="emissions_horizon_years",
        type=float,
        metavar="YEARS",
        default=d.get("emissions_horizon_years", None),
        help=(
            "Horizon (in years) over which the emissions-ray NPV factor "
            "is computed.  Unset = perpetuity (NPV = 1 / discount_rate). "
            "Only consumed by plp2gtopt's boundary-cut writer when "
            "--only-emissions is also set.  See issue #520."
        ),
    )


# ---------------------------------------------------------------------------
# Individual flag registrars
# ---------------------------------------------------------------------------


def add_scale_objective_argument(
    parser: argparse.ArgumentParser,
    *,
    default: float = 1000.0,
) -> None:
    """Register ``--scale-objective`` (objective scaling factor).

    Emitted as ``options.model_options.scale_objective``.  The C++
    default is 1000 in monolithic mode and 1.0 in cascade/sddp; pass
    ``default=`` to reflect the converter's preferred baseline.
    """
    parser.add_argument(
        "--scale-objective",
        dest="scale_objective",
        type=float,
        metavar="FACTOR",
        default=default,
        help=("objective function scaling factor. (default: %(default)s)"),
    )


def add_demand_fail_cost_argument(
    parser: argparse.ArgumentParser,
    *,
    default: float | None = 1000.0,
    help_text: str | None = None,
) -> None:
    """Register ``--demand-fail-cost`` (unserved-load penalty in $/MWh).

    Emitted as ``options.model_options.demand_fail_cost``.  ``default``
    of ``None`` signals "auto-derive from the case" — converters that
    can extract a per-case penalty (e.g. plp2gtopt's FALLA centrals)
    should pass ``default=None`` and substitute the derived value
    after parsing.
    """
    parser.add_argument(
        "--demand-fail-cost",
        dest="demand_fail_cost",
        type=float,
        metavar="COST",
        default=default,
        help=(
            help_text
            if help_text is not None
            else ("cost penalty for demand curtailment in $/MWh (default: %(default)s)")
        ),
    )


def add_use_kirchhoff_argument(
    parser: argparse.ArgumentParser,
    *,
    default: bool = True,
) -> None:
    """Register ``--use-kirchhoff`` / ``--no-use-kirchhoff``.

    Emitted as ``options.model_options.use_kirchhoff``.  ``True`` by
    default — DC-OPF with voltage angles is the standard formulation.
    """
    parser.add_argument(
        "-k",
        "--use-kirchhoff",
        dest="use_kirchhoff",
        action=argparse.BooleanOptionalAction,
        default=default,
        help="enable Kirchhoff voltage-law constraints (default: %(default)s)",
    )


def add_use_single_bus_argument(
    parser: argparse.ArgumentParser,
    *,
    default: bool | None = None,
    dialect: str = "boolean_optional",
) -> None:
    """Register ``--use-single-bus`` (copper-plate collapse).

    Two dialects to match the existing converters:

    * ``"boolean_optional"`` (plp2gtopt): ``BooleanOptionalAction``
      with ``-b`` short flag and tri-state ``default=None`` semantics
      (the converter auto-picks single-bus when the case has zero
      transmission lines, multi-bus otherwise).  ``--no-use-single-bus``
      explicitly forces multi-bus.

    * ``"store_true"`` (plexos2gtopt): plain boolean flag that toggles
      single-bus mode on when present.  No short flag, no auto-detect.

    Emitted as ``options.use_single_bus`` (or whatever the converter's
    writer hands to gtopt).
    """
    if dialect == "boolean_optional":
        parser.add_argument(
            "-b",
            "--use-single-bus",
            dest="use_single_bus",
            action=argparse.BooleanOptionalAction,
            default=default,
            help=(
                "use single-bus (copper-plate) mode; pass --no-use-single-bus "
                "to force the multi-bus network "
                "(default: auto — single-bus when the parsed PLP case has 0 "
                "transmission lines, multi-bus otherwise)"
            ),
        )
    elif dialect == "store_true":
        parser.add_argument(
            "--use-single-bus",
            action="store_true",
            help="collapse the multi-bus topology to a single bus (copperplate)",
        )
    else:
        raise ValueError(
            f"add_use_single_bus_argument: unknown dialect {dialect!r}; "
            "expected 'boolean_optional' or 'store_true'"
        )


def add_line_losses_mode_argument(
    parser: argparse.ArgumentParser,
    *,
    default: str | None = None,
) -> None:
    """Register ``--line-losses-mode`` (PWL/linear loss formulation).

    Choice list mirrors the C++ ``LineLossesMode`` enum
    (:data:`LINE_LOSSES_MODE_CHOICES`).  Emitted as
    ``options.model_options.line_losses_mode``.  ``default=None``
    leaves the field unset so gtopt picks ``adaptive``.
    """
    parser.add_argument(
        "--line-losses-mode",
        dest="line_losses_mode",
        metavar="MODE",
        default=default,
        choices=list(LINE_LOSSES_MODE_CHOICES),
        help=(
            "transmission-line loss model emitted as "
            "model_options.line_losses_mode. 'adaptive' (gtopt default) "
            "picks the smallest-LP PWL model — `piecewise` for fixed-"
            "capacity lines, `bidirectional` for expandable ones. "
            "'piecewise_direct' mirrors PLP `genpdlin.f` (per-segment "
            "bus stamps, no loss rows) at the cost of 2·K segment cols "
            "per direction — use for PLP LP-diff parity. "
            "'tangent_signed_flow' (Coffrin-Van Hentenryck 2014) uses a "
            "single signed-flow column + K outer-approximation tangents "
            "and a |f|-aux chord upper bound — strongest LP relaxation, "
            "no bidirectional-flow degeneracy. "
            "(default: not set — gtopt picks 'adaptive')"
        ),
    )


def add_loss_cost_eps_argument(
    parser: argparse.ArgumentParser,
    *,
    default: float | None = None,
    dialect: str = "plp",
) -> None:
    """Register ``--loss-cost-eps`` (per-direction loss column cost).

    Two dialects:

    * ``"plp"`` (plp2gtopt): ``default=None`` + ``metavar='EPS'`` —
      caller decides the default.  ``plp2gtopt._parsers`` passes
      ``default=0.1`` so PLP-derived runs ship with a strictly
      degeneracy-breaking value out of the box; pass ``--loss-cost-eps
      0`` to opt out.

    * ``"plexos"`` (plexos2gtopt): ``default=0.0`` and no metavar —
      always emitted in the planning JSON, even at the legacy 0.0.

    Emitted as ``options.model_options.loss_cost_eps``.
    """
    if dialect == "plp":
        parser.add_argument(
            "--loss-cost-eps",
            dest="loss_cost_eps",
            type=float,
            default=default,
            metavar="EPS",
            help=(
                "Small positive cost ($/MWh) stamped on every per-direction "
                "loss column (loss_p / loss_n) of PWL-loss lines. Strictly "
                "breaks the pure LP-relax bidirectional-flow degeneracy: "
                "the LP picks single-direction dispatch among primal-optimal "
                "solutions sharing the same net flow. Recommended: 1e-6 — "
                "essentially zero objective impact yet eliminates the "
                "residual phantom bidirectional flow. Emitted as "
                "options.model_options.loss_cost_eps. "
                "(default: not set — gtopt picks 0.0, legacy behaviour)"
            ),
        )
    elif dialect == "plexos":
        eff_default = 0.0 if default is None else default
        parser.add_argument(
            "--loss-cost-eps",
            type=float,
            default=eff_default,
            help=(
                "Small positive cost ($/MWh) stamped on every per-direction "
                "loss column (``loss_p`` / ``loss_n``) of PWL-loss lines.  "
                "Strictly breaks the pure LP-relax bidirectional-flow "
                "degeneracy: the LP picks single-direction dispatch among "
                "primal-optimal solutions sharing the same net flow.  "
                "Recommended: ``1e-6`` — essentially zero objective impact "
                "(well below LP optimality tolerance) yet eliminates the "
                "residual ~1-11%% phantom bidirectional flow that survives "
                "the ``piecewise → bidirectional`` wrapping.  Default 0.0 "
                "preserves legacy behaviour.  Emitted as the global "
                "``options.model_options.loss_cost_eps`` field — every "
                "PWL/bidirectional line inherits the same ε."
            ),
        )
    else:
        raise ValueError(
            f"add_loss_cost_eps_argument: unknown dialect {dialect!r}; "
            "expected 'plp' or 'plexos'"
        )


def add_lift_line_caps_argument(
    parser: argparse.ArgumentParser,
    *,
    default: str | None = None,
    dialect: str = "plp",
) -> None:
    """Register ``--lift-line-caps`` (per-line cap-lift list).

    Two dialects to match the existing converters:

    * ``"plp"`` (plp2gtopt): a comma-separated list of line names
      (optionally ``Name:Factor``) widening the ``loss_envelope`` for
      named lines.  ``default=None`` — no lift unless requested.

    * ``"plexos"`` (plexos2gtopt): a comma-separated list of line
      names to demote from PLEXOS EL=1 (hard cap) down to EL=0 (no
      cap).  ``default='Capricornio110->LaNegra110'`` ships a single
      curated entry for the CEN PCP weekly bundle.

    Emitted differently in each writer; see the respective converter
    for the post-parse interpretation.
    """
    if dialect == "plp":
        parser.add_argument(
            "--lift-line-caps",
            dest="lift_line_caps",
            metavar="NAMES",
            default=default,
            help=(
                "Comma-separated list of Line names whose loss_envelope is "
                "widened beyond tmax_normal to cover the overload band the "
                "LP can actually flow into under PWL-loss relaxation. "
                "Format: 'L1:FACTOR,L2:FACTOR' (FACTOR multiplies tmax_ab); "
                "or 'L1,L2' to use the converter's default factor (2.0). "
                "Emitted as per-line Line.loss_envelope. Useful when the "
                "default envelope [0, tmax_normal] under-approximates the "
                "true loss curve in the overload band, producing inflated "
                "secant losses or phantom flow."
            ),
        )
    elif dialect == "plexos":
        eff_default = "Capricornio110->LaNegra110" if default is None else default
        parser.add_argument(
            "--lift-line-caps",
            type=str,
            default=eff_default,
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
    else:
        raise ValueError(
            f"add_lift_line_caps_argument: unknown dialect {dialect!r}; "
            "expected 'plp' or 'plexos'"
        )


def add_aperture_chunk_size_argument(
    parser: argparse.ArgumentParser,
    *,
    default: int | None = None,
) -> None:
    """Register ``--aperture-chunk-size`` (SDDP chunk granularity).

    Emitted as ``options.sddp_options.aperture_chunk_size``.  Integer
    or ``None`` (auto): 0/unset = auto, 1 = legacy 1-per-task,
    > 1 = K-per-task, -1 = fully serial per scene.
    """
    parser.add_argument(
        "--aperture-chunk-size",
        dest="aperture_chunk_size",
        type=int,
        default=default,
        metavar="K",
        help=(
            "SDDP chunked aperture pass: K apertures solved serially per "
            "task on a shared LP clone (warm-start reuse). 0/unset = auto "
            "(currently resolves to 1, empirically fastest under the "
            "parallel-safe manual-clone path on juan/IPLP-scale workloads), "
            "1 = legacy 1-task-per-aperture, > 1 = K per task, "
            "-1 = fully serial per scene. Emitted as "
            "options.sddp_options.aperture_chunk_size."
        ),
    )


# ---------------------------------------------------------------------------
# Output selection (--write-out)
# ---------------------------------------------------------------------------


def add_write_out_argument(
    parser: argparse.ArgumentParser,
    *,
    default: str | None = None,
) -> None:
    """Register ``--write-out`` (output selection passed through to gtopt).

    The CLI value is forwarded verbatim to the planning JSON's
    ``options.write_out`` field; gtopt parses it via
    ``parse_output_selection`` (see ``include/gtopt/planning_enums.hpp``).

    Recognised tokens:

    * ``sol`` / ``solution``  — primal column values
    * ``dual``                — row duals
    * ``rc`` / ``reduced_cost`` — column reduced costs
    * ``extras``              — secondary slack / shadow / per-element
                                 detail columns
    * ``all``                  — alias for ``sol,dual,rc,extras``

    Element-class restriction with ``rc:Generator,Line`` etc.:
    space-free, comma-separated list of element class names appended
    after a colon limits the bit to those classes (others stay off).
    The default :data:`DEFAULT_WRITE_OUT` is exactly what
    ``gtopt_marginal_units`` needs — lean enough not to blow up disk
    on 2-year runs.
    """
    parser.add_argument(
        "--write-out",
        dest="write_out",
        metavar="SPEC",
        default=DEFAULT_WRITE_OUT if default is None else default,
        help=(
            "gtopt output selection (passed through to "
            "``options.write_out``).  Comma-separated tokens: "
            "``sol`` / ``dual`` / ``rc`` / ``extras`` / ``all``, optionally "
            "restricted to a class list via ``rc:Generator,Line``.  "
            "Default: %(default)r — exactly what gtopt_marginal_units, "
            "loss-audit, and the LMP-attribution pipelines need."
        ),
    )


# ---------------------------------------------------------------------------
# Grouped registrar
# ---------------------------------------------------------------------------


def add_common_arguments(
    parser: argparse.ArgumentParser,
    *,
    defaults: dict[str, object] | None = None,
    dialects: dict[str, str] | None = None,
    skip: set[str] | None = None,
) -> None:
    """Install every canonical gtopt flag on *parser* in one call.

    Parameters
    ----------
    parser
        argparse parser to register the flags on.
    defaults
        Per-flag default overrides keyed by ``dest`` name
        (``scale_objective``, ``demand_fail_cost``, ``use_kirchhoff``,
        ``use_single_bus``, ``line_losses_mode``, ``loss_cost_eps``,
        ``lift_line_caps``, ``aperture_chunk_size``).
    dialects
        Per-flag dialect overrides for the multi-dialect flags
        (``use_single_bus``, ``loss_cost_eps``, ``lift_line_caps``).
    skip
        Set of ``dest`` names to omit (useful when the converter
        already declares one of these via a converter-specific
        registrar and only needs the rest).
    """
    defaults = defaults or {}
    dialects = dialects or {}
    skip = skip or set()

    if "scale_objective" not in skip:
        add_scale_objective_argument(
            parser,
            default=float(defaults.get("scale_objective", 1000.0)),  # type: ignore[arg-type]
        )
    if "demand_fail_cost" not in skip:
        dfc = defaults.get("demand_fail_cost", 1000.0)
        add_demand_fail_cost_argument(
            parser,
            default=None if dfc is None else float(dfc),  # type: ignore[arg-type]
        )
    if "use_kirchhoff" not in skip:
        add_use_kirchhoff_argument(
            parser, default=bool(defaults.get("use_kirchhoff", True))
        )
    if "use_single_bus" not in skip:
        add_use_single_bus_argument(
            parser,
            default=defaults.get("use_single_bus"),  # type: ignore[arg-type]
            dialect=dialects.get("use_single_bus", "boolean_optional"),
        )
    if "line_losses_mode" not in skip:
        add_line_losses_mode_argument(
            parser,
            default=defaults.get("line_losses_mode"),  # type: ignore[arg-type]
        )
    if "loss_cost_eps" not in skip:
        add_loss_cost_eps_argument(
            parser,
            default=defaults.get("loss_cost_eps"),  # type: ignore[arg-type]
            dialect=dialects.get("loss_cost_eps", "plp"),
        )
    if "lift_line_caps" not in skip:
        add_lift_line_caps_argument(
            parser,
            default=defaults.get("lift_line_caps"),  # type: ignore[arg-type]
            dialect=dialects.get("lift_line_caps", "plp"),
        )
    if "aperture_chunk_size" not in skip:
        add_aperture_chunk_size_argument(
            parser,
            default=defaults.get("aperture_chunk_size"),  # type: ignore[arg-type]
        )
    if "write_out" not in skip:
        add_write_out_argument(
            parser,
            default=defaults.get("write_out"),  # type: ignore[arg-type]
        )
