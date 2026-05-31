"""Build a gtopt JSON planning from a :class:`PlexosCase`.

The output schema matches the small reference cases under
``cases/c0`` and ``cases/ieee_4b_ori`` — three top-level keys
``options``, ``simulation`` and ``system`` — so it can be solved by
``gtopt --lp-only`` directly without further post-processing.

The simulation is always a single-scenario / single-stage /
24-hourly-block daily layout (matching the CEN PCP horizon and the
``tools/ucjl2gtopt.py`` golden fixtures). Each per-class
``build_*_array`` function is isolated so per-class refinements stay
local.
"""

from __future__ import annotations

import csv
import json
import logging
import re
import shutil
from pathlib import Path
from collections.abc import Iterable, Iterator
from typing import Any

from .entities import (
    BatterySpec,
    BoundaryCutSpec,
    BundleSpec,
    CommitmentSpec,
    DecisionVariableSpec,
    DemandSpec,
    FlowRightSpec,
    FlowSpec,
    FuelSpec,
    GeneratorSpec,
    JunctionSpec,
    LineSpec,
    NodeSpec,
    PlantSpec,
    PlexosCase,
    ReservoirSpec,
    ReserveProvisionSpec,
    ReserveSpec,
    TurbineSpec,
    UserConstraintSpec,
    WaterwaySpec,
)
from .uc_families import UC_FAMILY_NAMES, uc_family  # re-exported (see __all__)


logger = logging.getLogger(__name__)


# CEN PCP horizon: one operating day = 24 hourly blocks.
DEFAULT_BLOCK_COUNT = 24
DEFAULT_BLOCK_DURATION_H = 1.0

# Single objective-scale knob driving both ``model_options.scale_objective``
# (LP objective coefficient divisor — gtopt divides every cost by this so
# the backend sees physical-cost / scale) AND the FCF ``alpha_fcf`` column
# scale ``_FCF_SCALE_ALPHA`` (objective coefficient on the alpha column,
# which carries ``future_cost / scale_alpha`` in physical terms).  Keeping
# the two in sync at 1000 means the alpha column's LP cost matches the
# rest of the objective's scaling regime — same gradient magnitude across
# every cost term, no asymmetric kappa contribution from a tiny alpha
# coefficient.  Tunable in one place; both downstream emissions read it.
_DEFAULT_OBJ_SCALE = 1000.0

# Soft-cap parameters for ex-PLEXOS-EL=0 lines (see ``build_line_array``).
# Each soft-capped line gets a FREE band up to ``normal × rating`` and the
# ``_LINE_OVERLOAD_PENALTY`` $/MWh only on flow between ``normal × rating``
# and the hard cap ``hard × rating``.  Raising the free threshold to 2×
# means the typical PLEXOS over-use (radial 110 kV at ~1.3–1.6×) flows
# free — no penalty, no LMP inflation — while a hard cap at 5× still blocks
# the DC-OPF teleport.  Lines named in ``--lift-line-caps`` (the radial
# corridors PLEXOS itself runs hardest, e.g. Capricornio110->LaNegra110 at
# 2.7×) get a wider free band (4×) and a 10× hard cap instead of being
# uncapped — so they carry PLEXOS-like flow for free yet can't teleport.
_LINE_SOFT_NORMAL_FACTOR = 2.0
_LINE_SOFT_HARD_FACTOR = 5.0
_LINE_LIFTED_NORMAL_FACTOR = 4.0
_LINE_LIFTED_HARD_FACTOR = 10.0
# The soft-cap overload penalty ($/MWh on flow between the rating and the
# 3× hard cap) is COMPUTED per bundle, not hard-coded: it is
# ``_compute_default_slack_cost(demands, gens) / _LINE_OVERLOAD_DIVISOR``,
# i.e. one quarter of the calibrated slack cost
# ``(min demand.fcost + max gen.gcost) / 2`` (~$285 → ~$71/MWh).  The ÷4
# lets the LP push roughly four "jumps" into an ex-EL0 line's soft band
# before it ever prefers shedding load (and it stays < demand_fail_cost).
_LINE_OVERLOAD_DIVISOR = 4.0

# Fallback productibility used when ``Hydro_EfficiencyIncr.csv`` lacks
# a value for a given Storage; matches the global irrigation default
# (see feedback_irrigation_design).
DEFAULT_FP_MED = 1.0  # MW per m³/s


def build_options(
    bundle: BundleSpec,
    *,
    use_single_bus: bool = False,
    demand_fail_cost: float = 1000.0,
) -> dict[str, Any]:
    """Map :class:`BundleSpec` onto gtopt's ``options`` block.

    Layout matches the ``cases/c0`` reference: top-level options carry
    I/O / discount-rate / directory settings; ``model_options`` is a
    nested object carrying the LP-shape flags (``use_single_bus``,
    ``use_kirchhoff``, ``scale_objective``, ``demand_fail_cost``).

    ``demand_fail_cost`` defaults to 1000 $/MWh.  When PLEXOS
    Region.VoLL is set on every region with a consistent value, the
    converter overrides this default with the PLEXOS VoLL — matching
    the system-wide Value of Lost Load that PLEXOS uses for demand
    curtailment penalty.  CEN PCP carries 467.19 $/MWh on both
    Central-Southern and Northern regions.
    """
    _ = bundle  # reserved for future use (bundle date stamps, calendar tags)
    return {
        "annual_discount_rate": 0.0,
        # ``input_directory`` is "." so all relative paths in the JSON
        # (parquet input files under ``Generator/`` etc. AND the
        # auto-discovered ``solvers/<solver_name>.prm`` parameter file
        # written next to the JSON by ``install_solver_param_files``)
        # resolve relative to the CASE directory the JSON lives in.
        #
        # Without this, gtopt's ``prepare_matrix_options`` defaults
        # ``input_directory`` to ``"input"`` (per
        # ``source/gtopt_lp_runner.cpp``) and looks for the tuned
        # ``cplex.prm`` at ``./input/solvers/cplex.prm`` instead of
        # ``./solvers/cplex.prm`` — the file is silently missed and
        # the curated ``Gomory = 2`` / ``MIPGap = 0.01`` overrides
        # never reach CPLEX.  Mirrors plp2gtopt's behaviour at
        # ``plp2gtopt/gtopt_writer.py:1195-1200``.
        "input_directory": ".",
        "input_format": "parquet",
        "output_format": "parquet",
        "output_compression": "snappy",
        # Emit the full output surface by default so downstream tools
        # (``compare_with_plexos``, the loss-audit pipeline, anything
        # that reads ``Line/loss_sol.parquet`` or
        # ``Generator/heat_rate_slack_sol.parquet``) have what they
        # need without re-running every solve.  Per
        # ``include/gtopt/planning_enums.hpp:429``,
        # ``OutputFlags::all`` is ``solution | dual | reduced_cost |
        # extras`` — i.e. it *does* include the ``extras`` bit that
        # gates ``loss_sol`` (see ``source/line_lp.cpp::add_to_output``
        # at the ``add_col_sol_extras(LossName, ...)`` call) and
        # ``heat_rate_slack`` (see
        # ``source/generator_lp.cpp::add_to_output`` for the
        # ``--write-out ...,extras:Generator`` opt-in).  Without this
        # the converter-emitted JSON would inherit gtopt's empty
        # default, which produces flowp/flown but no consolidated
        # loss stream → forces the loss-audit path to re-derive every
        # loss from ``R/V² · f² · dur`` and miss the LP's internal
        # PWL secant value entirely.
        "write_out": "all",
        # NOTE: ``lp_matrix_options.equilibration_method`` intentionally
        # left UNSET — the default Ruiz scaling rescales binary
        # commitment column upper bounds (e.g. ``commitment_status_X``
        # from [0, 1] to [0, 38.58] on CEN PCP weekly).  This is a
        # known correctness bug (task #50), but switching to
        # ``row_max``/``none`` here regresses production: the correctly-
        # bounded LP exposes latent structural infeasibilities in
        # PLEXOS RegRange UCs that the Ruiz-rescaled LP solves "around"
        # (the LP "solves" on a semantically-wrong problem with
        # fractional commitments up to 38.58, but it solves).  Tracked
        # follow-up: make PLEXOS-side RegRange UCs soft like the CSF
        # MinProvision fix (task #51) so the correctly-scaled LP can
        # absorb the structural infeasibility at a high $/MWh penalty.
        "model_options": {
            "use_single_bus": use_single_bus,
            "use_kirchhoff": not use_single_bus,
            "scale_objective": _DEFAULT_OBJ_SCALE,
            "demand_fail_cost": demand_fail_cost,
        },
        # Enable the backend solver log by default.  Two knobs are
        # needed (they are independent):
        #   * ``log_mode = 1`` (SolverLogMode.detailed) — makes
        #     ``PlanningLP`` call ``set_log_file`` so the backend writes a
        #     per-(scene, phase) file ``<log_directory>/<solver>_sc0_ph0.log``.
        #     Without this the solver is silent regardless of log_level.
        #   * ``log_level = 1`` — verbosity inside that file (CPLEX
        #     screen-indicator / MIP display level).
        # Together they capture the branch-and-bound trace (presolve, root
        # relaxation, nodes / incumbent / gap) — essential for diagnosing
        # MIP convergence on the PLEXOS reproduction.
        "solver_options": {
            "log_mode": 1,
            "log_level": 1,
            # Point gtopt at the curated CPLEX param file that
            # ``install_solver_param_files`` copies next to the JSON
            # (``solvers/cplex.prm``).  Setting ``param_file`` explicitly
            # bypasses gtopt's auto-discovery (which only fires when the
            # user passes ``--solver cplex`` — ``opts.solver`` is empty
            # under the default auto-detect path, so
            # ``prepare_matrix_options`` skips the
            # ``<input_directory>/solvers/<solver_name>.prm`` lookup at
            # ``source/gtopt_lp_runner.cpp:587``).  With this set the
            # curated overrides — ``MIP Cuts Gomory = 2`` and
            # ``MIP Tolerances MIPGap = 0.01`` — reach CPLEX regardless
            # of whether the user typed ``--solver cplex``.  Path is
            # relative to ``input_directory`` (``.`` above) so it
            # resolves to ``<case-dir>/solvers/cplex.prm``.
            "param_file": "solvers/cplex.prm",
        },
    }


def build_simulation(
    bundle: BundleSpec,
    *,
    block_count: int | None = None,
    block_duration_h: float = DEFAULT_BLOCK_DURATION_H,
) -> dict[str, Any]:
    """Emit the (1 scenario × 1 stage × N blocks) chronological skeleton.

    Matches both ``cases/c0`` and ``tools/ucjl2gtopt.py``: gtopt's
    simulation block needs only ``block_array``, ``stage_array``,
    ``scenario_array``. Scenes and phases are inferred by the solver
    when absent (single scene, single phase).

    When ``block_count`` is unset the function derives it from
    ``bundle.block_layout`` (PLEXOS-native mode — each block's
    duration = ``len(intervals_in_block)`` hours) or from
    ``bundle.n_days × bundle.step_count`` (hourly mode, 168-block week
    when ``n_days = 7``).  An explicit ``block_count`` overrides both.
    """
    if block_count is None:
        if bundle.block_layout:
            block_array = [
                {"uid": k + 1, "duration": float(len(intervals))}
                for k, intervals in enumerate(bundle.block_layout)
            ]
            block_count = len(block_array)
            stage_array = [
                {
                    "uid": 1,
                    "first_block": 0,
                    "count_block": block_count,
                    "active": 1,
                    "chronological": True,
                }
            ]
            scenario_array = [{"uid": 1, "probability_factor": 1.0}]
            return {
                "block_array": block_array,
                "stage_array": stage_array,
                "scenario_array": scenario_array,
            }
        block_count = bundle.step_count * bundle.n_days
    block_array = [
        {"uid": h + 1, "duration": block_duration_h} for h in range(block_count)
    ]
    stage_array = [
        {
            "uid": 1,
            "first_block": 0,
            "count_block": block_count,
            "active": 1,
            # ``chronological: true`` is required for Commitment LP rows
            # (commitment_lp.cpp:52 early-returns for non-chronological
            # stages, leaving status/startup/shutdown columns unbuilt
            # and breaking any UserConstraint that references them).
            # Matches the ``tools/ucjl2gtopt.py`` golden fixture shape.
            "chronological": True,
        }
    ]
    scenario_array = [{"uid": 1, "probability_factor": 1.0}]
    return {
        "block_array": block_array,
        "stage_array": stage_array,
        "scenario_array": scenario_array,
    }


def build_bus_array(nodes: tuple[NodeSpec, ...]) -> list[dict[str, Any]]:
    """One bus entry per :class:`NodeSpec` — just ``uid`` and ``name``."""
    out: list[dict[str, Any]] = []
    for i, node in enumerate(nodes):
        out.append({"uid": i + 1, "name": node.name})
    return out


#: Virtual fuel name used when a generator ships piecewise heat-rate
#: data but no Fuel object membership (118-Bus convention with
#: per-generator ``Fuel Price`` on the System→Generators collection).
#: gtopt's LP requires a Fuel reference for ``heat_rate_segments`` to
#: contribute to cost; we synthesise this fuel with ``price = 1`` and
#: pre-multiply the segment slopes by the generator's fuel-price
#: override so the LP sees the right $/MWh.
VIRTUAL_FUEL_NAME = "_VIRTUAL_FUEL_UNIT_PRICE"


def _flatten_positive(seq: Iterable[Any]) -> Iterator[float]:
    """Yield positive floats from a (possibly nested) sequence."""
    for x in seq:
        if isinstance(x, list):
            yield from _flatten_positive(x)
        elif x is not None and x > 0:
            yield float(x)


def soft_penalty_cost(
    gcost_values: Iterable[Any],
    voll_values: Iterable[Any],
    *,
    override: float | None = None,
) -> float:
    """The single system-wide soft-penalty $/MWh used for BOTH the
    forced-`pmin` (``Generator.pmin_fcost``) and the soft line-overload /
    EL=1 slack cost.

        soft_penalty = min(max(gcost) + 1, min(VoLL) - 1)

    ``max(gcost) + 1`` makes running a unit STRICTLY cheaper than
    leaving its forced floor unserved / shedding into the soft band;
    the ``min(VoLL) - 1`` cap keeps the penalty below the cheapest
    unserved-demand cost so load-serving always outranks it.  ``gcost``
    is the emitted generator-cost field (not full SRMC, per design).
    ``override`` (CLI ``--soft-penalty-cost``) forces an explicit value
    for both consumers.
    """
    if override is not None and override > 0.0:
        return override
    gcosts = list(_flatten_positive(gcost_values))
    penalty = (max(gcosts) + 1.0) if gcosts else 1.0
    volls = list(_flatten_positive(voll_values))
    if volls:
        penalty = min(penalty, min(volls) - 1.0)
    return penalty


def build_generator_array(
    generators: tuple[GeneratorSpec, ...],
    fuels: tuple[FuelSpec, ...] = (),
    generators_with_commitment: frozenset[str] = frozenset(),
    *,
    block_layout: tuple[tuple[int, ...], ...] = (),
    demand_voll: float | None = None,
    soft_penalty_override: float | None = None,
) -> list[dict[str, Any]]:
    """One generator entry per :class:`GeneratorSpec`.

    Cost wiring (in priority order):

    1. **Piecewise heat rate** — when ``pmax_segments`` /
       ``heat_rate_segments`` are populated (PLEXOS quadratic form,
       e.g. 118-Bus), emit the arrays plus a ``fuel`` reference so
       gtopt computes ``fuel.price × heat_rate_segments[k]`` per
       segment. For 118-Bus the converter synthesises a single
       ``VIRTUAL_FUEL_NAME`` Fuel with ``price = 1`` and pre-multiplies
       each segment slope by the generator's ``fuel_price_override``.
    2. **Scalar gcost** — ``gcost = heat_rate × fuel_price + vom_charge``
       (the default path used by CEN PCP and RTS-96).

    For renewables (no Fuel membership) only ``vom_charge`` survives —
    PLEXOS conventionally reports zero VO&M for solar/wind/RoR.

    ``Generator.pmin`` is the LP-level lower bound on the generator's
    dispatch column.  When the generator carries a Commitment, gtopt's
    ``commitment_lp.cpp`` rewrites the static col floor: it reads the
    original ``pmin``, places it on the linking row
    ``generation - pmin × u ≥ 0``, then resets the col lowb to 0
    (commitment_lp.cpp:389).  In other words the Commitment "gates" the
    Generator's ``pmin`` exactly like ``Converter.commitment`` gates
    the inner ``Generator.{pmin,pmax}`` (converter.hpp:90-106) — the
    pmin only fires when the unit is committed.

    PLEXOS "Min Stable Level" is per-unit (intended to fire only when
    committed); ``generators_with_commitment`` lists every generator
    that will receive a Commitment object in the JSON.  For any
    generator *not* in that set, we publish ``pmin = 0`` to avoid the
    forced-floor LP infeasibility (e.g. solar at midnight, wind under
    pmax = 0 hours).
    """
    fuel_price = {f.name: f.price for f in fuels}
    out: list[dict[str, Any]] = []
    # Forced-dispatch (non-renewable Fixed Load) entries collected here;
    # their soft-`pmin` penalty is assigned AFTER the loop, once every
    # entry's ``gcost`` field is known.  The penalty is the single
    # system-wide ``soft_penalty_cost(...)`` value (shared with the line
    # overload / EL=1 slack cost) = ``min(max(gcost)+1, min(VoLL)-1)``.
    forced_entries: list[dict[str, Any]] = []
    for i, gen in enumerate(generators):
        # Resolve the primary fuel price (used for both scalar gcost
        # and segment pre-multiplication).
        if gen.fuel_price_override > 0.0:
            primary_price = gen.fuel_price_override
            primary_fuel: str | None = None
        else:
            primary_fuel = gen.fuel_names[0] if gen.fuel_names else None
            primary_price = fuel_price.get(primary_fuel, 0.0) if primary_fuel else 0.0
        # Generator.pmin is the always-on hard floor.  The parser
        # already populates this correctly (= 0 for CEN PCP, since
        # PLEXOS Min Stable Level is per-unit when-committed and
        # travels via Commitment.pmin instead).  Pass through
        # unmodified.
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": gen.name,
            "bus": gen.bus_name,
        }
        # ``generator.pmin`` is the ALWAYS-ON hard floor — emit it only
        # when a real floor exists (> 0).  For CEN PCP it is always 0
        # (no always-on floor; commitment-conditional floors live on
        # ``Commitment.pmin``, and must-take renewables/RoR keep pmin=0
        # in the FixedLoad block below = curtailable), so it is normally
        # omitted rather than written as a redundant ``"pmin": 0``.
        if gen.pmin and gen.pmin > 0.0:
            entry["pmin"] = gen.pmin
        if gen.heat_rate_segments and gen.pmax_segments:
            # Piecewise path: emit segments + fuel ref. When there's
            # no real Fuel object (118-Bus), point at the virtual
            # unit-price fuel and pre-multiply slopes by the gen's
            # per-unit fuel price.  Add the additive VO&M as gcost.
            if primary_fuel is None:
                entry["fuel"] = VIRTUAL_FUEL_NAME
                segments_scaled = tuple(
                    s * primary_price for s in gen.heat_rate_segments
                )
            else:
                entry["fuel"] = primary_fuel
                segments_scaled = gen.heat_rate_segments
            entry["pmax_segments"] = list(gen.pmax_segments)
            entry["heat_rate_segments"] = list(segments_scaled)
            entry["gcost"] = gen.vom_charge + gen.fuel_transport
        elif primary_fuel is not None and gen.heat_rate > 0.0:
            # Scalar path WITH a known Fuel + heat_rate — emit the
            # explicit FK so gtopt's `FuelLP::add_to_lp` can find
            # this generator when building the per-stage offtake
            # cap row (PR #487 + #489).  gtopt computes
            #
            #   effective_gcost = fuel.price × heat_rate + gcost
            #
            # at LP-build, so we pass the *non-fuel* part of the
            # variable cost as ``gcost`` and let gtopt resolve the
            # fuel-price contribution from the Fuel element.  The
            # numerical result is identical to the legacy
            # pre-baked path below.
            entry["fuel"] = primary_fuel
            entry["heat_rate"] = gen.heat_rate
            entry["gcost"] = gen.vom_charge + gen.fuel_transport
        else:
            # Legacy baked-gcost path: no Fuel FK (renewables, units
            # without a Fuel membership in PLEXOS, virtual gens).
            # Everything is collapsed into a single scalar coefficient
            # on `generation` — the Fuel.max_offtake cap row will not
            # apply to these gens, which is correct: they don't draw
            # from a constrained fuel band.
            gcost = gen.heat_rate * primary_price + gen.vom_charge + gen.fuel_transport
            entry["gcost"] = gcost
        # When the per-hour rating profile actually varies (renewable
        # availability, scheduled maintenance) emit the profile as a
        # [[stage_blocks]] matrix so the LP honours it block-by-block.
        # Constant profiles collapse to the scalar max.  Under
        # ``--horizon-mode plexos`` the 168-hour profile is aggregated
        # to 111 block-mean values to line up with the simulation's
        # block_array.
        if gen.pmax_profile and (max(gen.pmax_profile) != min(gen.pmax_profile)):
            profile = (
                _aggregate_to_blocks(gen.pmax_profile, block_layout, reducer="mean")
                if block_layout
                else list(gen.pmax_profile)
            )
            entry["pmax"] = [profile]
        else:
            entry["pmax"] = gen.pmax

        # PLEXOS ``Auxiliary Use`` (Gen_AuxUse.csv) → gtopt
        # ``Generator.lossfactor``: the LP injects ``(1 − lossfactor) ×
        # generation`` to the bus (generator_lp: ``brow[gcol] = 1 −
        # lossfactor``), so station-service self-consumption is modelled
        # exactly — gross generation is still dispatched and costed at
        # ``gcost``, but only the net reaches the grid.  Preferred over a
        # ``pmax`` derate, which would wrongly shrink the gross cap and
        # price fuel on net output.
        if 0.0 < gen.aux_use < 1.0:
            entry["lossfactor"] = gen.aux_use

        # PLEXOS ``Generator.Fixed Load`` (Gen_FixedLoad.csv): per-period
        # *required generation* (PLEXOS fixes the generation variable to
        # this value).  We honour it ONLY at blocks where
        # ``fixed_load[t] > 0``; a zero-valued FixedLoad row means "no
        # forced-dispatch constraint at this period" (= free dispatch
        # within ``[Commitment.pmin, Gen_Rating[t]]``), NOT "force
        # dispatch to 0".  Verified on CEN PCP COCHRANE_1: 168/168
        # FixedLoad rows present, only hours 1-3 have positive values
        # (the start-up trajectory PLEXOS uses to mirror commitment state
        # from the prior week); hours 4-168 are FixedLoad=0 and the unit
        # dispatches actively (13.6 GWh / week, peaking at 244.86 MW) —
        # interpreting the zero rows as ``pmax=0`` zeros out a 244 MW
        # thermal unit for 165h and yields a primal-infeasible LP.
        #
        # The forced value is mapped tech-dependently:
        #
        #   * Non-renewable (fuel + heat rate, piecewise segments, or a
        #     per-unit fuel-price override → real marginal cost): pin
        #     ``pmin = pmax = fixed_load`` (hard equality), matching
        #     PLEXOS's fixed-generation semantics.  COCHRANE_1's hours
        #     1-3 trajectory is honoured exactly.
        #   * Renewable / RoR (no fuel, zero heat rate → zero marginal
        #     cost): emit a curtailable cap ``pmin = 0, pmax = fixed_load``.
        #     gcost≈0 keeps the LP maxing the free energy under merit
        #     order (matching PLEXOS) while allowing curtailment under
        #     congestion — avoiding a hard pmin=pmax infeasibility for
        #     units whose output a transmission/commitment limit forces
        #     lower.
        #
        # Where FixedLoad=0 we restore the original ``pmax`` (Gen_Rating-
        # derived block-profile or scalar) and leave ``pmin`` at the
        # gen's always-on floor (``gen.pmin``, usually 0 — Commitment.pmin
        # handles the commitment-conditional floor).
        if gen.fixed_load_profile:
            fl_profile = (
                _aggregate_to_blocks(
                    gen.fixed_load_profile, block_layout, reducer="mean"
                )
                if block_layout
                else list(gen.fixed_load_profile)
            )
            if fl_profile and any(v > 0.0 for v in fl_profile):
                # Recover the block-aggregated pmax profile we already
                # computed above so we can keep the original cap on
                # zero-FixedLoad blocks.  Scalar-pmax gens broadcast.
                if isinstance(entry["pmax"], list):
                    base_pmax = list(entry["pmax"][0])
                else:
                    base_pmax = [float(entry["pmax"])] * len(fl_profile)
                base_pmin = float(gen.pmin or 0.0)
                # A real marginal energy cost marks a dispatchable
                # thermal/fuel unit whose Fixed Load is a forced-dispatch
                # / commitment trajectory PLEXOS pins exactly (pmin=pmax).
                # No fuel and zero heat rate ⇒ zero-cost renewable / RoR ⇒
                # curtailable cap (pmin=0).  See the block comment above.
                has_fuel_cost = bool(gen.heat_rate_segments and gen.pmax_segments) or (
                    gen.heat_rate > 0.0
                    and (primary_fuel is not None or gen.fuel_price_override > 0.0)
                )
                pmin_blocks: list[float] = []
                pmax_blocks: list[float] = []
                for t, fl in enumerate(fl_profile):
                    if fl > 0.0:
                        # Non-renewable: honour the forced value EXACTLY
                        # (pmin=pmax=fl).  Renewable/RoR: curtailable cap
                        # (pmin=0, pmax=fl) — gcost≈0 still maxes the free
                        # energy under merit order while avoiding the hard
                        # pmin=pmax infeasibility under congestion.
                        pmin_blocks.append(fl if has_fuel_cost else 0.0)
                        pmax_blocks.append(fl)
                    else:
                        pmin_blocks.append(base_pmin)
                        # Keep the cap from Gen_Rating; index-safe even
                        # if the two arrays differ in length (rare).
                        pmax_blocks.append(base_pmax[t] if t < len(base_pmax) else 0.0)
                # Emit the cap (scalar when uniform, else per-block).
                if min(pmax_blocks) == max(pmax_blocks):
                    entry["pmax"] = pmax_blocks[0]
                else:
                    entry["pmax"] = [pmax_blocks]
                # Emit pmin only if a real floor remains (>0); the
                # curtailable renewables/RoR keep pmin at its default 0.
                if any(v > 0.0 for v in pmin_blocks):
                    if min(pmin_blocks) == max(pmin_blocks):
                        entry["pmin"] = pmin_blocks[0]
                    else:
                        entry["pmin"] = [pmin_blocks]
                else:
                    entry.pop("pmin", None)
                # Soft-pmin penalty (non-renewables only): a hard
                # pmin=pmax forced floor can collide with a transmission /
                # commitment / ramp limit and render the LP infeasible.
                # Track forced (non-renewable) Fixed Load entries; the
                # ``pmin_fcost`` soft-floor penalty is assigned post-loop
                # from the system-wide ``soft_penalty_cost(...)`` value so
                # gtopt relaxes the hard floor via an ``unserved`` slack
                # instead of going infeasible under congestion.
                if has_fuel_cost:
                    forced_entries.append(entry)

        # PLEXOS ``Generator.Initial Generation`` is captured in
        # ``GeneratorSpec.initial_generation`` for downstream tooling
        # but NOT emitted to the gtopt JSON: the Generator schema has
        # no ``initial_generation`` field yet (rolling-horizon /
        # cascade-state work is a follow-up).  Emitting it here
        # triggers gtopt's strict daw::json "Could not find member"
        # failure at LP build.

        # Conversion provenance (F5): coarse tech tag + standardized
        # ``description`` documenting source + key field units.  No C++
        # schema change — Generator already carries ``type``/``description``.
        has_fuel = (
            bool(gen.fuel_names)
            or gen.heat_rate > 0.0
            or (gen.fuel_price_override > 0.0)
        )
        entry["type"] = "thermal" if has_fuel else "renewable"
        entry["description"] = (
            f"PLEXOS Generator '{gen.name}' at bus '{gen.bus_name}' → gtopt "
            f"Generator; pmin/pmax [MW], gcost [$/MWh], heat_rate "
            f"[fuel-unit/MWh]; (File: DBSEN_PRGDIARIO.xml + Gen_*.csv)"
        )

        out.append(entry)

    # Assign the shared system-wide soft-`pmin` penalty to every forced
    # (non-renewable Fixed Load) entry: min(max(gcost)+1, min(VoLL)-1),
    # or the explicit override.  Same value + option as the line
    # overload / EL=1 slack cost (see ``soft_penalty_cost``).
    if forced_entries:
        penalty = soft_penalty_cost(
            (e.get("gcost") for e in out),
            [demand_voll] if demand_voll is not None else [],
            override=soft_penalty_override,
        )
        if penalty > 0.0:
            for e in forced_entries:
                e["pmin_fcost"] = penalty
    return out


def _needs_virtual_fuel(generators: tuple[GeneratorSpec, ...]) -> bool:
    """True iff any generator emits piecewise segments without a real Fuel."""
    return any(
        g.heat_rate_segments
        and g.pmax_segments
        and not g.fuel_names
        and g.fuel_price_override > 0.0
        for g in generators
    )


def augment_el1_with_soft_caps(
    line_entries: list[dict[str, Any]],
    *,
    overload_penalty: float,
    headroom_factor: float = 3.0,
) -> int:
    """Convert every EL=1 line into a soft-capped line.

    Uses gtopt's native ``Line.tmax_normal_*`` + ``overload_penalty``
    primitive (line_lp.cpp emits an overload-slack column per direction
    when both are set).  Below ``tmax_normal_*`` the LP pays nothing
    extra; above it the LP pays ``overload_penalty`` per MWh up to the
    hard cap ``tmax_*``.  That's a true piecewise soft cap on a SINGLE
    Line entity — cleaner than appending parallel slack lines (which
    distort the Kirchhoff cycle structure with every extra branch).

    For each EL=1 entry with a non-zero ``tmax_ab``:
      * ``tmax_normal_ab`` ← original ``tmax_ab``         (soft target)
      * ``tmax_ab``        ← ``headroom_factor × original`` (hard cap)
      * Same for the B→A leg when ``tmax_ba`` is set.
      * ``overload_penalty`` ← the caller-supplied per-MWh penalty.
      * ``enforce_level`` stays 1 (the new ``tmax_ab`` is the hard cap;
        the LP only ever pushes flow into the soft band when economics
        demand it).

    ``headroom_factor`` default ``3.0``: gives the LP up to 3× rated
    capacity at penalty, which covers every observed CEN PCP overflow
    (largest case Capricornio ≈ 2.7× rated).  Capped at 3× because
    gtopt's loss-PWL envelope is anchored on ``tmax_ab`` (the hard
    cap), so larger headrooms stretch the PWL across the over-capacity
    band and under-estimate I²R losses for flow near the rated point.
    A future gtopt change (decoupling the loss envelope from the flow
    cap via a separate ``block_loss_envelope_*`` parameter to
    ``line_losses::add_block``) would let us safely set headroom to
    ``+∞`` without distorting the loss model.

    Recommended ``overload_penalty`` on CEN PCP bundles:
        avg(min(demand.fcost), max(generator.gcost))
    (~$285.81/MWh on the 2026-04-22 bundle).  High enough that
    re-dispatch wins whenever possible, low enough that the LP doesn't
    prefer dropping demand over routing flow at the soft penalty.

    Returns the number of EL=1 entries softened (each mutated in
    place — no new line entries are appended).
    """
    n_softened = 0
    for ln in line_entries:
        if ln.get("enforce_level") != 1:
            continue
        rated_ab = ln.get("tmax_ab")
        if rated_ab is None:
            continue
        if isinstance(rated_ab, (int, float)) and rated_ab <= 0:
            continue
        # Capture the ORIGINAL rating as the loss-PWL envelope BEFORE
        # inflating the hard cap.  gtopt's loss-PWL envelope is decoupled
        # from the flow cap via ``Line.loss_envelope`` (line_losses.cpp):
        # pinning it to the original rating keeps the K loss segments
        # concentrated over the realistic loading band instead of being
        # stretched across the headroom-inflated cap, where they'd be
        # coarse and under-resolve I²R losses near the rated point.  Only
        # set it for lines that actually carry a piecewise loss model
        # (``line_losses_mode == 'piecewise'``); for the rest it's inert.
        # Use the larger of the two original directional ratings so a
        # single envelope covers both legs.
        orig_ratings = [
            v
            for v in (ln.get("tmax_ab"), ln.get("tmax_ba"))
            if isinstance(v, (int, float)) and v > 0
        ]
        if orig_ratings and ln.get("line_losses_mode") == "piecewise":
            # Refinement A (gated by ``GTOPT_LOSS_EXTEND_OVERLOAD=1``,
            # i.e. ``--loss-extend-overload``): extend the PWL envelope
            # by the same headroom factor the LP uses for the soft-cap
            # overload band, so loss-curve resolution covers the
            # actually-reachable flow range instead of just the rated
            # point.  Default off — keeps the historical pre-2026-05-29
            # behaviour where ``loss_envelope`` is pinned to the original
            # rating and the rare overload band is handled by linear
            # extrapolation of the last segment slope.
            import os as _os_inner

            _extend = _os_inner.environ.get(
                "GTOPT_LOSS_EXTEND_OVERLOAD", "0"
            ).strip() in ("1", "true", "yes", "on")
            ln["loss_envelope"] = max(orig_ratings) * (
                headroom_factor if _extend else 1.0
            )
        # A→B leg
        if "tmax_ab" in ln:
            ln["tmax_normal_ab"] = ln["tmax_ab"]
            if isinstance(ln["tmax_ab"], (int, float)):
                ln["tmax_ab"] = ln["tmax_ab"] * headroom_factor
        # B→A leg (only if it's set)
        if "tmax_ba" in ln:
            ln["tmax_normal_ba"] = ln["tmax_ba"]
            if isinstance(ln["tmax_ba"], (int, float)):
                ln["tmax_ba"] = ln["tmax_ba"] * headroom_factor
        ln["overload_penalty"] = overload_penalty
        n_softened += 1
    return n_softened


def _compute_default_slack_cost(
    demand_entries: list[dict[str, Any]],
    generator_entries: list[dict[str, Any]],
    *,
    fallback: float = 285.81,
    override: float | None = None,
) -> float:
    """Default soft line-overload / EL=1 slack ``tcost`` from the
    bundle's own demand fail-cost and generator-cost values.

    Uses the SAME formula and override option as the forced-`pmin`
    penalty (see ``soft_penalty_cost``):

        slack_cost = min(max(gcost) + 1, min(VoLL) - 1)

    High enough that re-dispatch beats the soft band, capped below the
    cheapest unserved-demand cost so load-serving always outranks it.
    Falls back to ``fallback`` if the generator-cost pool is empty.
    """
    gcosts = [g.get("gcost") for g in generator_entries if g.get("gcost") is not None]
    if not any(_flatten_positive(gcosts)):
        return fallback
    return soft_penalty_cost(
        gcosts,
        [d.get("fcost") for d in demand_entries if d.get("fcost") is not None],
        override=override,
    )


def _int_loss_env(key: str, default: int) -> int:
    """Read a positive int from a ``GTOPT_*`` env var, else ``default``."""
    import os as _os

    try:
        v = int(_os.environ.get(key, str(default)))
    except ValueError:
        return default
    return v if v >= 1 else default


def _resolve_loss_layout(line: Any) -> tuple[str, int]:
    """Resolve ``(loss_pwl_layout, loss_segments)`` for one lossy line.

    Every line uses the ``GTOPT_LOSS_PWL_LAYOUT`` base layout
    (``--loss-pwl-layout``, default ``dynamic``), EXCEPT lines explicitly
    named in ``GTOPT_LOSS_TANGENT_LINES`` (``--loss-tangent-lines``),
    which get the ``tangent`` layout with ``GTOPT_NSEG_TANGENT`` segments.

    Segment count precedence:

      1. ``line.loss_segments`` if set (> 0) by ``extract_lines`` via the
         cube-root adaptive rule (``--loss-error-pct``).  Per-line K, the
         normal path on bundles converted post-2026-05-29.
      2. ``GTOPT_NSEG_LOSSES`` env var (``--nseg-losses``, default 6)
         applied uniformly when the adaptive rule was disabled
         (``--loss-error-pct 0``) or the LineSpec carries no override
         (older JSON pre-dating the field).

    The legacy R·P² percentile RANKING (``--loss-tangent-top-pct`` +
    ``_loss_proxy`` / ``_tangent_loss_cutoff``) was REMOVED: the
    ``midpoint`` de-bias + the per-line ``loss_envelope`` decoupling match
    PLEXOS losses to within ~2% at K=4 without the MIP-heavy hybrid tangent
    tier (CEN PCP daily case), so the loading-classified ranking is no
    longer needed.  The explicit ``--loss-tangent-lines`` escape hatch
    remains for callers who want tangent on specific named lines.
    """
    import os as _os

    forced = {
        n.strip()
        for n in _os.environ.get("GTOPT_LOSS_TANGENT_LINES", "").split(",")
        if n.strip()
    }
    if line.name in forced:
        return "tangent", _int_loss_env("GTOPT_NSEG_TANGENT", 6)
    base = _os.environ.get("GTOPT_LOSS_PWL_LAYOUT", "midpoint")
    if base not in ("uniform", "equal_error", "midpoint", "tangent", "dynamic"):
        base = "uniform"
    # Per-line LineSpec.loss_pwl_layout takes precedence over the
    # global base — set by ``_apply_dynamic_loss_layout`` when the
    # user picked ``--loss-pwl-layout dynamic``.  The dynamic rule
    # auto-assigns either ``"uniform"`` or ``"midpoint"`` per line so
    # the system-wide signed mean error stays within
    # ``--loss-error-pct``.  Empty string ⇒ no override; fall back to
    # the global base (which itself rewrites ``"dynamic"`` → uniform
    # default if the user forgot to also enable the dynamic rule
    # path, keeping the LP buildable instead of erroring).
    per_line_layout = getattr(line, "loss_pwl_layout", "") or ""
    resolved_layout = per_line_layout if per_line_layout else base
    if resolved_layout == "dynamic":
        # No per-line override AND the global base is still
        # ``"dynamic"`` ⇒ user requested dynamic but the rule didn't
        # stamp anything (no lossy lines, or dynamic dispatch was
        # skipped).  Fall back to the safer uniform default.
        resolved_layout = "uniform"
    # Prefer the per-line LineSpec.loss_segments override (set by
    # ``_apply_adaptive_loss_segments`` in ``extract_lines``).  Fall back
    # to the uniform env-var path when not set — this branch fires for
    # legacy JSON bundles pre-2026-05-29 (no per-line override) and for
    # direct ``build_line_array`` callers (e.g. unit tests) that build
    # LineSpec without going through ``extract_lines``.  Default of 4
    # matches the historic uniform-K default (the new ``6`` default
    # only applies to the *adaptive ceiling* inside
    # ``_apply_adaptive_loss_segments``).
    per_line_k = getattr(line, "loss_segments", 0) or 0
    if per_line_k > 0:
        return resolved_layout, int(per_line_k)
    return resolved_layout, _int_loss_env("GTOPT_NSEG_LOSSES", 4)


def _scale_tmax(value: Any, factor: float) -> Any:
    """Scale a ``tmax_*`` entry by ``factor``.

    Handles both the scalar form (``float``) and the per-block matrix
    form (``[[v, ...]]`` emitted for DLR lines), leaving the JSON shape
    unchanged so the soft (``tmax_normal_*``) and hard (``tmax_*``)
    limits stay congruent.
    """
    if isinstance(value, (int, float)):
        return value * factor
    return [[x * factor for x in row] for row in value]


def build_line_array(
    lines: tuple[LineSpec, ...],
    *,
    block_layout: tuple[tuple[int, ...], ...] = (),
    overload_penalty: float = 285.81 / _LINE_OVERLOAD_DIVISOR,
) -> list[dict[str, Any]]:
    """One line entry per :class:`LineSpec`.

    gtopt's `Line` carries forward/reverse capacity as ``tmax_ab`` /
    ``tmax_ba`` (both non-negative MW). PLEXOS' ``Lin_MinRating.csv``
    ships the reverse-flow limit as a negative number; we flip the
    sign and multiply by the parallel-line count.

    When :attr:`LineSpec.tmax_ab_profile` carries a non-constant
    per-hour profile (DLR — Dynamic Line Rating), the writer
    aggregates that profile to ``block_layout`` and emits
    ``tmax_ab`` as a ``[[per-block]]`` matrix so the LP honours the
    higher daytime rating block-by-block.  Constant profiles
    collapse to the scalar (peak) tmax_ab.
    """
    # Loss layout is resolved per-line in the loop below via
    # ``_resolve_loss_layout`` (base layout for all lines, default
    # ``midpoint``; tangent only for explicitly named ``--loss-tangent-lines``).
    #
    # ``GTOPT_LOSS_EXTEND_OVERLOAD`` (``--loss-extend-overload``) — when
    # set, EXTEND each soft-cap line's PWL ``loss_envelope`` by the same
    # headroom factor the LP uses for the soft-cap overload band, so the
    # K segments (sized by ``_apply_adaptive_loss_segments`` for the
    # extended envelope) actually have envelope room to cover the
    # overload region.  Off → envelope stays pinned to the original
    # rating (the pre-2026-05-29 default).
    #
    # Bug history (#44): the writer's soft-cap inline block at line
    # 1015+ used to hardcode ``loss_envelope = orig_env`` without
    # consulting the env var, so the parsers side that DID consult it
    # to over-allocate K_i ended up wasting segments — K was sized for
    # ``2× tmax`` but the C++ side saw ``loss_envelope = tmax`` and
    # collapsed the segments back into ``[0, tmax]``.  This block now
    # mirrors the K-sizing path's flag read.
    import os as _os_inner_writer

    _extend_overload = _os_inner_writer.environ.get(
        "GTOPT_LOSS_EXTEND_OVERLOAD", "0"
    ).strip() in ("1", "true", "yes", "on")
    out: list[dict[str, Any]] = []
    for i, line in enumerate(lines):
        # Parser (`extract_lines`) already clamps hour-0 units to 1 and
        # logs a WARN; this assert pins the invariant so the writer's
        # branchless `tmax * units` math doesn't silently collapse a
        # bad input to zero.
        assert line.units >= 1, (
            f"line '{line.name}' has units={line.units}; "
            f"parser should have clamped this to 1 with a warning"
        )
        units = line.units
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": line.name,
            "bus_a": line.bus_from,
            "bus_b": line.bus_to,
        }
        # PLEXOS Enforce Limits → gtopt ``Line.enforce_level``
        # (same int 0/1/2 semantics as PLEXOS):
        #   0 = Never enforce — emit ``tmax_ab`` (loss-segment
        #       discretization needs a real envelope, otherwise
        #       ``seg_width = DblMax / nseg`` produces meaningless
        #       PWL segments) PLUS ``enforce_level = 0`` so the LP
        #       doesn't bind on the rating.
        #   1 = Voltage-conditional — PLEXOS empirically enforces
        #       these caps in CEN PCP weekly economic dispatch (88
        #       of 89 EL=1 lines with flow stay at-or-below cap),
        #       but allows exceptions on lines where AC voltage
        #       support requires flow above the cap (Capricornio110→
        #       LaNegra110 — radial 110 kV, no parallel path, must
        #       carry 200+ MW to serve Antofagasta load).  We
        #       default EL=1 to ``enforce_level = 1`` (treated as
        #       hard cap in our LP since no AC iteration available)
        #       and let the caller pass an explicit lift-list
        #       (``--lift-line-caps``) to flip specific names to
        #       ``enforce_level = 0``.
        #   2 = Always enforce — hard cap (historical behaviour and
        #       gtopt's schema default).
        # Max Rating (pid 1882) is not used as a soft/hard pair —
        # data-quality check flagged sentinel values (Antofag110->
        # Desalant110: 17.5× Max Flow).  Max Flow is the single
        # hard cap.
        if line.tmax_ab > 0.0:
            ab_profile = line.tmax_ab_profile
            ba_profile = line.tmin_ab_profile

            if ab_profile and min(ab_profile) != max(ab_profile):
                profile = (
                    _aggregate_to_blocks(ab_profile, block_layout, reducer="mean")
                    if block_layout
                    else list(ab_profile)
                )
                entry["tmax_ab"] = [[v * units for v in profile]]
            else:
                entry["tmax_ab"] = line.tmax_ab * units

            if ba_profile and min(ba_profile) != max(ba_profile):
                profile = (
                    _aggregate_to_blocks(ba_profile, block_layout, reducer="mean")
                    if block_layout
                    else list(ba_profile)
                )
                entry["tmax_ba"] = [[abs(v) * units for v in profile]]
            else:
                entry["tmax_ba"] = (
                    abs(line.tmin_ab) * units if line.tmin_ab else line.tmax_ab * units
                )

            # Emit ``enforce_level`` only when it differs from the
            # gtopt default of 2 (always enforce).  Keeps the JSON
            # compact for the 63 EL=2 lines on CEN PCP and makes the
            # explicit EL=0 / lifted EL=1 lines stand out.
            if line.enforce_limits < 2:
                entry["enforce_level"] = line.enforce_limits
            # Per-block in-service flag from PLEXOS ``Lin_Units.csv``
            # (maintenance / forced-outage windows).  Aggregate the
            # per-hour 0/1 profile to blocks with the ``min`` reducer (a
            # block is OUT if ANY of its hours is out — the conservative
            # choice for line availability), and emit ``Line.in_service``
            # only when at least one block is out.  An always-in profile
            # keeps the schema default (line in service everywhere).
            if line.in_service_profile:
                blk = (
                    _aggregate_to_blocks(
                        [float(v) for v in line.in_service_profile],
                        block_layout,
                        reducer="min",
                    )
                    if block_layout
                    else [float(v) for v in line.in_service_profile]
                )
                if any(v < 0.5 for v in blk):
                    entry["in_service"] = [[1 if v >= 0.5 else 0 for v in blk]]
            # Soft-cap the ex-PLEXOS-EL=0 lines (parser flagged
            # ``soft_cap`` and promoted them to enforce_level=1): free
            # flow up to the rating (``tmax_normal_*``), penalised
            # between the rating and ``_LINE_HEADROOM_FACTOR × rating``
            # (the new hard ``tmax_*``).  Mirrors PLEXOS, which leaves
            # these lines uncapped yet in practice runs them at most
            # ~2.7× rated; the penalty stops gtopt from teleporting tens
            # of GW across them while keeping the radial pockets PLEXOS
            # over-serves feasible.  Orig EL=1/EL=2 stay plain hard caps.
            if line.soft_cap:
                # Lifted corridors get the wider 4×/10× band; the rest 2×/5×.
                normal_f = (
                    _LINE_LIFTED_NORMAL_FACTOR
                    if line.soft_cap_lifted
                    else _LINE_SOFT_NORMAL_FACTOR
                )
                hard_f = (
                    _LINE_LIFTED_HARD_FACTOR
                    if line.soft_cap_lifted
                    else _LINE_SOFT_HARD_FACTOR
                )

                # Capture the ORIGINAL (pre-soft-cap) rating as the
                # loss-PWL envelope BEFORE inflating the hard cap.  gtopt
                # decouples the loss-PWL envelope from the flow cap via
                # ``Line.loss_envelope`` (line_losses.cpp): pinning it to
                # the original rating keeps the K loss segments
                # concentrated over the realistic loading band instead of
                # being stretched across the headroom-inflated cap (where
                # they would be coarse and under-resolve I²R losses near
                # the rated point).  Inert for non-piecewise lines.  Use
                # the larger original directional rating so a single
                # envelope covers both legs; the loss mode itself is
                # assigned further below.
                def _scalar_max(value: Any) -> float:
                    """Peak scalar of a tmax entry (scalar or [[matrix]])."""
                    if isinstance(value, (int, float)):
                        return float(value)
                    return max((float(x) for row in value for x in row), default=0.0)

                orig_env = 0.0
                if "tmax_ab" in entry:
                    rating_ab = entry["tmax_ab"]
                    orig_env = max(orig_env, _scalar_max(rating_ab))
                    entry["tmax_normal_ab"] = _scale_tmax(rating_ab, normal_f)
                    entry["tmax_ab"] = _scale_tmax(rating_ab, hard_f)
                if "tmax_ba" in entry:
                    rating_ba = entry["tmax_ba"]
                    orig_env = max(orig_env, _scalar_max(rating_ba))
                    entry["tmax_normal_ba"] = _scale_tmax(rating_ba, normal_f)
                    entry["tmax_ba"] = _scale_tmax(rating_ba, hard_f)
                if orig_env > 0.0:
                    # ``--loss-extend-overload`` (#44): when ON, extend
                    # the PWL envelope by the hard-cap headroom factor
                    # (2× for regular soft_cap, 4× for soft_cap_lifted)
                    # so the K segments — already sized for the wider
                    # envelope by ``_apply_adaptive_loss_segments`` —
                    # actually have envelope room to cover the soft-cap
                    # overload band.  OFF (default): keep the historical
                    # pinning to the original rating.
                    envelope_factor = hard_f if _extend_overload else 1.0
                    entry["loss_envelope"] = orig_env * envelope_factor
                entry["overload_penalty"] = overload_penalty
        if line.reactance > 0.0:
            entry["reactance"] = line.reactance
        # PLEXOS Resistance (pid 1888) → gtopt Line.resistance +
        # piecewise loss mode.  PLEXOS-CEN ships R in per-unit on
        # the system MVA base (same convention as Reactance), and
        # uses its built-in PWL approximation of the quadratic loss
        # curve P_loss = R · f² / V² with the default number of
        # tranches.  Mirror that with gtopt's ``piecewise`` mode
        # (single-direction PWL — Macedo et al. 2011) using 3
        # segments per line.  3 is a sane compromise between LP
        # size (3 extra variables + 1 loss-track row per block per
        # line) and accuracy — the PWL error vs the exact
        # quadratic at flow = f_max scales as 1/K, so 3 segments
        # cap the max error at ~17% of f_max, which is well within
        # the noise of PLEXOS's own approximation.
        if line.resistance > 0.0:
            entry["resistance"] = line.resistance
            # Voltage chosen for unit consistency, NOT physical V.
            # gtopt's loss formula is `P_loss = R · f² / V²`.  We
            # have R in p.u. on a 100 MVA base and f in MW.  For the
            # output to come out in MW we need V² = S_base = 100,
            # so the per-unit-of-V is V = √S_base = 10.  This is the
            # standard p.u.→engineering-unit trick: setting voltage =
            # √S_base makes the per-unit R produce MW-valued losses
            # without forcing every flow variable to be carried in
            # p.u.  Independently confirmed by sanity-check on the
            # northern corridor Capricornio110->LaNegra110:
            #   R = 0.0554 p.u., f_max ≈ 175 MW
            #   P_loss = 0.0554 × 175² / 100 ≈ 17 MW at max flow
            #   (~10% loss, in line with PLEXOS's 1.96 GWh / week
            #    on that line)
            entry["voltage"] = 10.0  # √S_base, S_base = 100 MVA
            # Loss mode: piecewise with 3 segments ONLY for lines
            # with an enforced capacity (EL=2 → tmax_ab present).
            # Lines with PLEXOS Enforce Limits ∈ {0, 1} have no
            # tmax_ab in the bundle (gtopt treats them as +∞) and
            # cannot be assigned a piecewise loss curve — the
            # segments would be unbounded.  Mapping them to linear
            # losses with a synthetic anchor (e.g. Lin_MaxRating)
            # produces unbounded losses when actual flow exceeds the
            # rating (a normal occurrence when the cap isn't
            # enforced): λ × |f| diverges from the physical
            # quadratic.  Cleaner to leave EL=0/1 lines lossless
            # than to model them with the wrong shape.  Net effect:
            # ~51 of 317 lines carry piecewise losses on CEN PCP
            # 2026-04-22 (the EL=2 set covers the most binding
            # international and inter-zonal interconnections).
            if "tmax_ab" in entry:
                entry["line_losses_mode"] = "piecewise"
                # Per-line segment count + layout, resolved from the
                # converter's loss env vars: the base layout
                # (``--loss-pwl-layout``, default ``dynamic``) with
                # ``--nseg-losses`` segments for every line, or ``tangent``
                # for lines explicitly named in ``--loss-tangent-lines``.
                layout, nseg = _resolve_loss_layout(line)
                entry["loss_segments"] = nseg
                # Emit ``loss_pwl_layout`` only when non-default (uniform
                # is gtopt's default) to keep the JSON minimal.
                if layout != "uniform":
                    entry["loss_pwl_layout"] = layout
                # Pin ``loss_envelope`` to the line's ORIGINAL rating
                # whenever it wasn't already set by the soft-cap block
                # above.  Without this, ``line_losses.cpp`` falls back
                # to a default envelope (~ ``tmax × 2``) which spreads
                # the K loss segments across DOUBLE the realistic
                # loading band → each segment under-resolves I²R losses
                # near the rated point → the LP picks a flow pattern
                # that over-counts losses on every high-capacity 500 kV
                # backbone line.  Regression history: the
                # ``project_loss_model_midpoint_envelope`` memory recorded
                # ``K5 midpoint+envelope → losses −4 % vs PLEXOS``, but
                # the K-sweep shows +19-29 % once this envelope is
                # missing on the 119 non-soft-capped lines (the entire
                # 500 kV mesh + key 220 kV transformers).
                if "loss_envelope" not in entry:

                    def _peak_tmax(v: Any) -> float:
                        """Peak rating across scalar OR DLR-profile tmax
                        (matches the soft-cap block's ``_scalar_max``
                        helper above): keep the highest value the line
                        is ever rated for so the loss-PWL envelope
                        covers the realistic loading band uniformly."""
                        if isinstance(v, (int, float)):
                            return float(v)
                        if isinstance(v, list):
                            try:
                                return max(
                                    (float(x) for row in v for x in row),
                                    default=0.0,
                                )
                            except TypeError:
                                # 1-D list (no outer brackets)
                                return max((float(x) for x in v), default=0.0)
                        return 0.0

                    env = max(
                        _peak_tmax(entry.get("tmax_ab", 0.0)),
                        _peak_tmax(entry.get("tmax_ba", 0.0)),
                    )
                    if env > 0.0:
                        entry["loss_envelope"] = env
        # PLEXOS Wheeling Charge ($/MWh) → gtopt Line.tcost.
        if line.wheeling_charge > 0.0:
            entry["tcost"] = line.wheeling_charge
        out.append(entry)
    return out


def _aggregate_to_blocks(
    hourly: tuple[float, ...] | list[float],
    block_layout: tuple[tuple[int, ...], ...],
    *,
    reducer: str = "mean",
) -> list[float]:
    """Collapse an ``n_days × 24``-element hourly profile to one value
    per PLEXOS block, using the layout's interval lists.

    ``block_layout[k]`` is the 1-indexed hourly intervals that
    constitute block ``k``.  ``hourly[i]`` is the value at 0-indexed
    hour ``i`` (so interval ``j`` → ``hourly[j - 1]``).

    Supported reducers:
      * ``mean`` — time-average (right for demand, hydro inflow,
        renewable capacity).
      * ``min`` — most restrictive (right for line capacity, units).
      * ``max`` — least restrictive (rare; e.g. headroom).
      * ``sum`` — total energy / count.
    """
    if not block_layout:
        return list(hourly)
    out: list[float] = []
    n = len(hourly)
    for intervals in block_layout:
        vals = [hourly[iv - 1] for iv in intervals if 1 <= iv <= n]
        if not vals:
            out.append(0.0)
            continue
        if reducer == "min":
            out.append(min(vals))
        elif reducer == "max":
            out.append(max(vals))
        elif reducer == "sum":
            out.append(sum(vals))
        else:  # mean
            out.append(sum(vals) / len(vals))
    return out


def _commit_status_code(v: int) -> float:
    """Map a PLEXOS ``Gen_Commit`` value to gtopt ``fixed_status``:
    ``+1 -> 1.0`` (pin u=1), ``0 -> 0.0`` (pin u=0), anything else
    (``-1``) -> ``-1.0`` — the out-of-``[0, 1]`` "no-pin" sentinel that
    ``commitment_lp.cpp`` reads as "leave this block's ``u`` free".
    """
    if v == 1:
        return 1.0
    if v == 0:
        return 0.0
    return -1.0


def _aggregate_commit_status(
    hourly: tuple[int, ...] | list[int],
    block_layout: tuple[tuple[int, ...], ...],
) -> list[float]:
    """Collapse an hourly PLEXOS commit-status profile (values in
    ``{-1, 0, +1}``) to one forced-status value per block for gtopt's
    ``Commitment.fixed_status``.

    Per block the modal hourly value wins; ties (including a block that
    straddles a forced-on/forced-off boundary) resolve to ``-1`` (free)
    so an ambiguous block is never pinned to the wrong status.  With no
    ``block_layout`` the hourly profile is mapped 1:1.
    """
    if not block_layout:
        return [_commit_status_code(int(v)) for v in hourly]
    out: list[float] = []
    n = len(hourly)
    for intervals in block_layout:
        vals = [int(hourly[iv - 1]) for iv in intervals if 1 <= iv <= n]
        if not vals:
            out.append(-1.0)
            continue
        counts = ((1, vals.count(1)), (0, vals.count(0)), (-1, vals.count(-1)))
        top = max(c for _, c in counts)
        winners = [v for v, c in counts if c == top]
        out.append(_commit_status_code(winners[0]) if len(winners) == 1 else -1.0)
    return out


def build_demand_array(
    demands: tuple[DemandSpec, ...],
    *,
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One demand entry per :class:`DemandSpec`, with inline ``lmax`` matrix.

    The PCP daily horizon fits comfortably inline (127 demands × 24
    blocks ≈ 24 kB of JSON); Parquet sidecar emission would only pay
    off for multi-stage horizons.

    When ``block_layout`` is non-empty (``--horizon-mode plexos``) the
    hourly profile is aggregated to one value per block (mean over the
    block's hourly intervals) so the demand vector lines up with the
    PLEXOS-shaped block_array.
    """
    out: list[dict[str, Any]] = []
    for i, dem in enumerate(demands):
        profile = (
            _aggregate_to_blocks(dem.lmax_profile, block_layout, reducer="mean")
            if block_layout
            else list(dem.lmax_profile)
        )
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": dem.name,
            "bus": dem.bus_name,
            # Inline lmax: gtopt expects [[stage_0_blocks], ...]; for the
            # PCP single-stage horizon this is a 1×N matrix.
            "lmax": [profile],
        }
        # Per-Region VoLL → per-Demand fcost.  Honours the literature-
        # audit fix that replaces the global ``max(VoLLs)`` collapse:
        # each Demand picks up its serving Region's curtailment
        # penalty natively; Demands without a matched Region inherit
        # the global ``model_options.demand_fail_cost`` (which we now
        # default to ``min(VoLLs)``).  fcost = 0 ⇒ omit the field so
        # gtopt's default is used.
        if dem.fcost > 0.0:
            entry["fcost"] = dem.fcost
        out.append(entry)
    return out


def build_battery_array(
    batteries: tuple[BatterySpec, ...],
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One battery entry per :class:`BatterySpec`.

    Emits the full battery schema (energy bounds, symmetric power
    rating, charge/discharge efficiency). Zero-energy batteries
    (Capacity property missing from t_data) are still emitted so the
    bus map stays consistent; the LP will see them with emax=0.

    Defensive clamp + WARNING: CEN PCP occasionally ships
    ``Initial Volume > Max Volume`` (e.g. BAT_ARICA: eini=76.388 MWh
    vs emax=2.0 MWh — likely a units mismatch between %SoC and MWh).
    Such a battery makes ``emin ≤ energy[0] = eini ≤ emax``
    impossible to honour and CPLEX flags ``battery_energy_*`` as
    infeasible at presolve.  Clamp ``eini`` into ``[emin, emax]`` and
    report the culprit so the user can audit the source.
    """
    out: list[dict[str, Any]] = []
    for i, bat in enumerate(batteries):
        eini = bat.eini
        if bat.emax > 0.0 and eini > bat.emax:
            logger.warning(
                "battery %s: Initial Volume (%.3f MWh) > Max Volume "
                "(%.3f MWh) — clamping eini to emax. Audit the bundle "
                "for unit / source mismatch (PLEXOS %% SoC vs MWh).",
                bat.name,
                eini,
                bat.emax,
            )
            eini = bat.emax
        elif eini < bat.emin:
            logger.warning(
                "battery %s: Initial Volume (%.3f MWh) < Min Volume "
                "(%.3f MWh) — clamping eini to emin.",
                bat.name,
                eini,
                bat.emin,
            )
            eini = bat.emin
        # End-of-horizon anchoring via per-block emin / emax profiles.
        # gtopt's legacy ``efin`` field is enforced as
        # ``vol_end >= efin`` (a lower bound) — for the common
        # ``efin = eini = 0`` configuration that's mathematically
        # redundant with the variable's natural bound, leaving the
        # energy-balance dual chain unanchored
        # (UPStorageBound_BAT_* duals cascading to -inf, verified
        # 2026-05-31 on v0407 LP-relax).
        #
        # Anchor the chain via per-block emin / emax HARD equalities
        # at both endpoints (BESS daily-cycle physics):
        #   * First block: emin = emax = eini  →  energy[1] = eini
        #   * Last block:  emin = emax = eini  →  energy[N] = eini
        #   * Other blocks: emin = bat.emin, emax = bat.emax (default)
        #
        # Both endpoints pinned to the PLEXOS-supplied initial SoC.
        # For batteries that PLEXOS initialises (``BESS_IniValue.csv``
        # > 0) this matches the natural daily-cycle return-to-start
        # behaviour.  For batteries with eini = 0 it forces the LP to
        # end empty too — combined with the self-discharge loss below,
        # the LP-relax dual cascade is cured for all batteries.
        # ``daily_cycle = False`` disables gtopt's auto-anchor.
        emin_field: Any = bat.emin
        emax_field: Any = bat.emax
        use_cycle_anchor = (
            bat.max_cycles_day > 0.0
            and bat.emax > 0.0
            and block_layout  # need to know n_blocks per stage
        )
        if use_cycle_anchor:
            n_blocks = len(block_layout)
            emin_blocks = [bat.emin] * n_blocks
            emax_blocks = [bat.emax] * n_blocks
            # Pin block 1 hard to eini.
            emin_blocks[0] = eini
            emax_blocks[0] = eini
            # Pin block N hard to eini — full daily-cycle equality.
            emin_blocks[-1] = eini
            emax_blocks[-1] = eini
            emin_field = [emin_blocks]
            emax_field = [emax_blocks]
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": bat.name,
            "bus": bat.bus_name,
            "emin": emin_field,
            "emax": emax_field,
            "eini": eini,
            "efin": bat.efin,
        }
        if use_cycle_anchor:
            # Disable gtopt's auto-cycle ``efin == eini`` anchor —
            # the per-block emin/emax profiles already encode the
            # daily-cycle physics.  Leaving ``daily_cycle = true``
            # (the BatteryLP default) would double-anchor and force
            # the LP into a strictly tighter region than PLEXOS.
            entry["daily_cycle"] = False
        if bat.pmax_discharge > 0.0:
            entry["pmax_discharge"] = bat.pmax_discharge
        if bat.pmax_charge > 0.0:
            entry["pmax_charge"] = bat.pmax_charge
        # PLEXOS ``Min Charge Level`` / ``Min Discharge Level`` are
        # *commitment-conditional* — they fire only when the battery
        # actively enters charge / discharge mode (u_charge=1 /
        # u_discharge=1).  gtopt mirrors this exactly via
        # ``Battery.commitment=True``: when set, ``System::expand_batteries()``
        # forwards the flag onto the synthetic ``Converter``, whose
        # ``add_to_lp`` introduces per-block binaries gating the C2
        # rows ``load >= lmin × u_charge`` / ``load <= lmax × u_charge``
        # (and the symmetric pair on the discharge side).  When pmin
        # > pmax (battery data anomaly, BAT_DEL_DESIERTO + BAT_TOCOPILLA
        # — Min level > Max Power) the LP simply forces u=0 at every
        # block, matching PLEXOS's "mode deactivated" interpretation.
        #
        # Trade-off: enabling commitment introduces integer columns,
        # switching the cell from LP to MIP.  For CEN PCP that's 2
        # batteries × 24 blocks × 2 binaries = 96 integers — trivial.
        pmin_c = bat.pmin_charge
        pmin_d = bat.pmin_discharge
        if pmin_c > 0.0:
            entry["pmin_charge"] = pmin_c
        if pmin_d > 0.0:
            entry["pmin_discharge"] = pmin_d
        if pmin_c > 0.0 or pmin_d > 0.0:
            entry["commitment"] = True
        if bat.output_efficiency != 1.0:
            entry["output_efficiency"] = bat.output_efficiency
        if bat.input_efficiency != 1.0:
            entry["input_efficiency"] = bat.input_efficiency
        # PLEXOS ``Max Cycles Day`` (= 1.0 for all CEN PCP batteries):
        # daily energy-throughput limit.  gtopt enforces the HARD row
        # ``Σ discharge·Δt ≤ N · capacity`` per day — so it needs an
        # explicit ``capacity`` (the usable energy = ``emax``) for the
        # RHS; without it the cap is unbounded and gtopt skips the row.
        if bat.max_cycles_day > 0.0 and bat.emax > 0.0:
            entry["capacity"] = bat.emax
            entry["max_cycles_day"] = bat.max_cycles_day
        # Default self-discharge for Li-ion BESS — gtopt's
        # ``Battery.annual_loss`` (p.u./year linear) drives the
        # energy-balance row coefficient
        # ``SoC[t+1] = SoC[t] × (1 − annual_loss / 8760) + flows``.
        #
        # Literature on Li-ion BESS self-discharge:
        #   * Battery University BU-802b: 0.35–2.5 %/month at 20 °C
        #   * NREL Energy Storage Database: LFP 1–3 %/month;
        #     NMC 2–5 %/month
        #   * IEA Battery Storage Roadmap 2024: 1–3 %/month typical
        #
        # 2 %/month is mid-range — representative of LFP cells (the
        # dominant grid-BESS chemistry in CEN deployments) at
        # ambient operating temperatures (northern Chile averages
        # ~25 °C).  Cumulative annual fraction lost:
        # ``1 − (1 − 0.02)^12 ≈ 0.215`` (21.5 %/year linear).
        #
        # Setting an explicit loss has TWO LP-conditioning benefits
        # beyond physical realism: (a) it gently penalises holding
        # energy in storage, regularising the energy-balance equality
        # chain and curing the LP-relax dual cascade we observe at
        # eini = 0 batteries; (b) it makes the writer's emitted JSON
        # self-documenting for downstream tools (gtopt_check, audit
        # scripts) that expect explicit physical parameters.
        #
        # Only emit when PLEXOS doesn't already ship its own
        # ``annual_loss`` value (which the converter pulls from
        # ``Battery.Self-discharge Rate`` if PLEXOS sets it — never
        # observed populated in v0407 but the safety check costs
        # nothing).
        if bat.max_cycles_day > 0.0 and bat.emax > 0.0:
            if "annual_loss" not in entry:
                entry["annual_loss"] = 0.215  # 2%/month → 21.5%/year
        out.append(entry)
    return out


def build_fuel_array(fuels: tuple[FuelSpec, ...]) -> list[dict[str, Any]]:
    """One fuel entry per :class:`FuelSpec`.

    Monthly ``Fuel_Price.csv`` is already collapsed to the day-of-
    bundle scalar by :func:`parsers.extract_fuels`; ``heat_content``
    stays at the parsed default (zero) unless the bundle ships a
    per-fuel ``Heat Content`` t_data row.

    When :attr:`FuelSpec.co2_rate` or :attr:`FuelSpec.co2_upstream_rate`
    is non-zero, a ``"emission_factors"`` array is emitted holding a
    single row tagged ``emission = "co2"``.  The C++ side multiplies
    the combustion + upstream rates by the matching
    :attr:`Generator.heat_rate` to recover the per-MWh emission rate
    consumed by ``EmissionZone`` constraints (see
    ``source/system.cpp::expand_fuel_emission_sources``).

    The ``emission`` tag here references an entry in the planning's
    ``emission_array`` — the converter does not currently emit that
    array itself, so callers that want a live CO₂ price must merge in
    an ``emission_array`` containing a row named ``"co2"`` via the
    standard JSON merge.  Without it, gtopt logs an unresolved-name
    warning and the emission row is dropped at LP-build time.
    """
    out: list[dict[str, Any]] = []
    for i, fuel in enumerate(fuels):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": fuel.name,
            "price": fuel.price,
            "heat_content": fuel.heat_content,
        }
        if fuel.co2_rate != 0.0 or fuel.co2_upstream_rate != 0.0:
            factor: dict[str, Any] = {"emission": "co2"}
            if fuel.co2_rate != 0.0:
                factor["combustion"] = fuel.co2_rate
            if fuel.co2_upstream_rate != 0.0:
                factor["upstream"] = fuel.co2_upstream_rate
            entry["emission_factors"] = [factor]
        # Weekly offtake cap — mirrors PLEXOS ``FueMaxOffWeek_<fuel>``
        # Constraint.  ``None`` means the fuel is absent from
        # ``Fuel_MaxOfftakeWeek.csv`` (no cap binds this bundle).
        # Explicit 0.0 IS emitted so PLEXOS's "shut on this week"
        # signal carries through — the gtopt Fuel.max_offtake row
        # then forces every generator on this fuel to dispatch 0
        # within the stage (cheaper than the legacy band-aid that
        # would have over-priced the band's marginal cost).
        if fuel.max_offtake is not None:
            entry["max_offtake"] = fuel.max_offtake
            # Use per-STAGE SUM mode (the gtopt default), NOT per-block.
            # `Fuel_MaxOfftakeWeek.csv` ships a WEEKLY total cap.
            # Enforcing it per-block (with uniform pro-rating by
            # block duration) collapses the constraint into an
            # effective per-hour power cap — semantically equivalent
            # to lowering `pmax`, which removes the LP's flexibility
            # to time-shift fuel use within the week.  Per-stage SUM
            # = `Σ_blocks heat_rate × gen × dur ≤ weekly_cap` keeps
            # the LP free to pick WHEN to burn the fuel, matching
            # PLEXOS's weekly-budget semantics.
            #
            # On CEN PCP weekly 2026-04-22, switching from per-block
            # to per-stage SUM was part of fixing the LNG +229%
            # over-dispatch (with the missing-gens fix in
            # parsers.py).
            # Leave ``max_offtake_per_block`` unset → gtopt FuelLP
            # defaults to per-stage SUM.
            #
            # ``max_offtake_cost`` makes the cap SOFT (a priced slack
            # column).  PLEXOS treats ``FueMaxOffWeek_*`` as soft and
            # violates it; a hard cap re-throttles LNG and forces coal.
            if fuel.max_offtake_cost is not None:
                entry["max_offtake_cost"] = fuel.max_offtake_cost
        # Min Offtake floor (PLEXOS pids 595-600 — bare /
        # Hour / Day / Week / Month / Year — folded into a horizon-wide
        # budget by the parser).  ``min_offtake_cost`` ships the
        # PLEXOS-faithful $1000/fuel-unit default when the bundle
        # populates a floor without an explicit penalty; the gtopt LP
        # model keeps the gtopt-native "unset ⇒ hard" convention so
        # the translation lives entirely at the conversion boundary.
        # Default per-stage SUM mode (matches the per-stage budget
        # semantics of the folded value).
        if fuel.min_offtake is not None:
            entry["min_offtake"] = fuel.min_offtake
            if fuel.min_offtake_cost is not None:
                entry["min_offtake_cost"] = fuel.min_offtake_cost
        out.append(entry)
    return out


def build_emission_array(fuels: tuple[FuelSpec, ...]) -> list[dict[str, Any]]:
    """Emit the ``emission_array`` pollutant definition(s) for CO₂.

    ``build_fuel_array`` tags each carbon-bearing fuel with an
    ``emission_factors`` row referencing the pollutant ``"co2"``; gtopt's LP
    build requires a matching ``emission_array`` entry or it drops the factor
    with a warning.  Emit a single ``{"uid": 1, "name": "co2"}`` pollutant
    definition whenever any fuel carries a CO₂ rate, so the per-fuel emission
    accounting becomes active.  Returns ``[]`` when no fuel emits CO₂ (the
    common CEN PCP case — carbon pricing off — keeps the JSON lean).

    An ``EmissionZone`` (cap / carbon price) is intentionally NOT synthesised
    here: this bundle ships no ``Emission`` objects, so there is no cap to
    convert.  When a bundle does, ``extract_emissions`` (TODO) feeds the zone.
    """
    if any(f.co2_rate != 0.0 or f.co2_upstream_rate != 0.0 for f in fuels):
        return [{"uid": 1, "name": "co2"}]
    return []


def build_reservoir_array(
    reservoirs: tuple[ReservoirSpec, ...],
    *,
    soft_efin_reservoirs: frozenset[str] = frozenset({"L_Maule"}),
) -> list[dict[str, Any]]:
    """One ``Reservoir`` per :class:`ReservoirSpec``.

    gtopt's Reservoir is attached to a Junction by name. We use the
    same name for both (the writer emits one Junction per Reservoir
    via :func:`build_junction_array`), so the ``junction`` field is a
    self-referential lookup.

    ## Spillway wiring (gtopt's built-in "internal drain")

    PLEXOS models *physical* spillways as ``Vert_*`` Waterway objects
    (Spanish *vertimiento* = spillage) draining from one reservoir to
    a downstream junction — e.g. ``Vert_PANGUE`` (PANGUE → ANGOSTURA).
    In CEN PCP, 7 / 11 real reservoirs carry such a Waterway: CIPRESES,
    ELTORO, MACHICURA, PANGUE, PEHUENCHE, POLCURA (via Vert_ANTUCO),
    RALCO.  The remaining 4 — ANGOSTURA, CANUTILLAR, COLBUN, RAPEL —
    are terminal plants whose spillage drains to ocean / river, which
    PLEXOS leaves out of its Waterway graph.

    PLEXOS also exposes a ``Spill Penalty`` / ``Non-physical Spill
    Penalty`` / ``Max Spill`` Storage property triple to model an
    *implicit* spill column on every reservoir, but all three are
    zero/unset in CEN PCP.  The PLP / SDDP convention matches: every
    reservoir has a "spill-to-sea" safety valve with a high penalty,
    so the LP can always balance volume even when ``Vert_*`` is
    absent or saturated.

    gtopt's storage layer ships exactly this — ``storage_lp.hpp:582``
    enables a per-block ``drain`` column on the energy balance row
    *iff* ``spillway_cost`` is set, with the column's upper bound
    defaulting to ``DblMax`` when ``spillway_capacity`` is unset
    (``storage_lp.hpp:714``).  So we only need to set the cost — the
    drain capacity is unbounded by default, matching the PLEXOS /
    PLP "non-physical spill" semantics.  Without this cost, terminal
    reservoirs overflow ``emax`` under natural inflow and CPLEX
    presolve flags ``reservoir_energy_*`` as infeasible.

    Note: ``extract_reservoirs`` reads the PLEXOS Storage ``Spill
    Penalty`` property into ``spill_penalty_per_mwh``, but every CEN
    PCP storage ships it at 0 / unset (verified 2026-05-20 against
    `DATOS20260422.zip.xz`).  Until a bundle arrives with a non-zero
    Spill Penalty, every reservoir takes the 1000 $/(m³/s·h) default.
    """
    out: list[dict[str, Any]] = []
    for i, res in enumerate(reservoirs):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": res.name,
            "junction": res.name,  # co-located junction (same name)
            "eini": res.eini,
            # PLEXOS native units: Storage volume in CMD (= cumec·day
            # = m³/s × 86,400 s = 86,400 m³), flow in cumec (m³/s).
            # Verified via t_unit: unit_id 24 = 'CMD' on Initial /
            # End / Min / Max Volume props (645/646/643/644);
            # unit_id 46 = '$/CMD' on Water Value (1101).
            #
            # Dimensional conversion ``m³/s · h → CMD`` is
            # ``3600/86400 = 1/24`` (s/h ÷ s/day).  PLP volumes in
            # hm³ use the struct default ``0.0036 = 3600/1e6``
            # instead.
            # Avoid the EXACT IEEE 754 bit pattern of 1.0/24.0
            # (= 0.041666666666666664), which triggers a 10× mutation
            # in daw::json's parser path on this specific value
            # (verified 2026-05-22 via the parse probe in
            # gtopt_json_io_parse.cpp; 1 ULP up or down doesn't
            # trigger).  Perturb the divisor by ~1e-13 so the parsed
            # double lands one ULP off the trigger bit pattern while
            # remaining dimensionally identical to 1/24 to double
            # precision.
            "flow_conversion_rate": 1.0 / 24.000000000001,
        }
        # Per-block profile takes precedence over the scalar — emitted
        # as the inline ``[[per-block]]`` matrix gtopt's ``Reservoir``
        # accepts for ``OptTBRealFieldSched`` fields.  Used for the
        # PLEXOS "static (physical) emin*/emax* in the first N-1
        # blocks + operational tight bound on the last block" pattern.
        if res.emin_profile:
            entry["emin"] = [list(res.emin_profile)]
        elif res.emin > 0.0:
            entry["emin"] = res.emin
        if res.emax_profile:
            entry["emax"] = [list(res.emax_profile)]
        elif res.emax > 0.0:
            entry["emax"] = res.emax
        elif res.eini == 0.0:
            # Run-of-river / pondage-like reservoir referenced by a
            # turbine (so the pondage demoter at extract_case can't
            # drop it).  Explicit ``emax = 0`` with ``eini = 0`` pins
            # the volume at zero every block, forcing ``inflow ==
            # outflow`` (pass-through).  Without this the writer
            # silently omits emax → gtopt treats the reservoir as
            # unbounded above, letting the LP accumulate water at the
            # node as a free time-shift buffer (verified 2026-05-22 on
            # CEN PCP: ISLA storage grew by 118 CMD over the week with
            # no physical justification, providing free cascade-water
            # arbitrage and inflating downstream hydro generation).
            entry["emax"] = 0.0
        # End-of-horizon storage target.  ``ReservoirSpec.efin`` is
        # populated from the LAST-day end-of-day floor in
        # ``Hydro_MinVolume.csv`` (see ``extract_reservoirs``).  Two
        # paths from here:
        #
        # 1. ``never_drain == True`` (PLEXOS sentinel Water Value
        #    ``1e+30``, e.g. ``L_Maule``): emit a HARD
        #    ``vol_end >= efin`` constraint with NO ``efin_cost``
        #    slack — the LP can't buy out of the sentinel at any
        #    finite price.
        # 2. Otherwise: SOFT slack priced at the storage's PLEXOS
        #    ``Water Value`` ($/GWh, since gtopt's per-PLEXOS-bundle
        #    ``efin`` is in PLEXOS native GWh-equivalent units, so
        #    ``efin_cost`` is also in $/GWh).  CEN PCP 2026-04-22
        #    ships 10,000 $/GWh on every dispatched reservoir.
        if res.efin > 0.0:
            entry["efin"] = res.efin
            # Per-reservoir SOFTENING opt-in (``soft_efin_reservoirs``
            # set, default ``{"L_Maule"}``).  Outside the set, ``efin``
            # is emitted WITHOUT ``efin_cost`` → gtopt builds a HARD
            # ``vol_end >= efin`` row that the LP MUST satisfy.
            #
            # Rationale: every reservoir with a finite ``Water Value``
            # used to be soft (efin_cost = water_value).  That let the
            # LP pay the per-GWh penalty (e.g. ELTORO at $10k) and
            # drain water past the published floor, generating
            # downstream cascade hydro for cheap.  CEN PCP 2026-04-22
            # gtopt drained ELTORO 128 GWh below its 12,079-GWh
            # target → $1.28M slack, freeing +20 GWh of ELTORO
            # dispatch + +5 GWh of cascade RUCUE generation that
            # PLEXOS doesn't take.  Restoring the HARD floor for
            # ELTORO (and other regular reservoirs) closes the
            # arbitrage, matching PLEXOS treatment.
            #
            # ``L_Maule`` keeps a SOFT efin because its PLEXOS
            # ``Water Value = 1e+30`` sentinel implies a never-drain
            # floor that the LP cannot reach without slack when
            # combined with the upstream ramp / FixedLoad tightening
            # (otherwise the L_Maule reservoir balance becomes the
            # dominant infeasibility row).  Pin its slack at a HIGH
            # cost so the LP respects it whenever physically possible.
            # ``efin_cost`` is intentionally NOT emitted: terminal
            # storage is now valued by the single end-of-horizon boundary
            # cut (FCF + per-reservoir water-value slopes from
            # ``Hydro_StoWaterValues.csv``, wired via
            # ``simulation.boundary_cuts_file``).  A per-reservoir
            # ``efin_cost`` would double-count that valuation, and the old
            # sentinel ``efin_cost = 1e6`` for never-drain reservoirs is
            # dropped per design: never_drain ⇒ no drain target priced,
            # no cost.  ``efin`` is still emitted, so the LP keeps a HARD
            # ``vol_end >= efin`` floor; the boundary cut prices storage
            # above that floor.
            _ = soft_efin_reservoirs  # retained for CLI compat; no longer gates efin_cost
        # Reservoir-internal drain is DISABLED by default, matching PLP's
        # convention: spillage leaves the basin via an explicit ``Vert_*``
        # Waterway routed to a ``<source>_ocean`` drain junction (added by
        # :func:`extract_waterways` + :func:`_is_sink_junction`), not via
        # an internal ``spillway_cost`` column on the storage balance row.
        # The previous default ($1000/MWh internal drain on every
        # reservoir) gave the LP two equivalent escape paths and let it
        # arbitrage between them under degeneracy.
        #
        # When PLEXOS does ship a per-storage ``Spill Penalty`` (currently
        # unset across CEN PCP), the extractor populates
        # ``spill_penalty_per_mwh`` and we honour it here.  Otherwise the
        # field is omitted and ``storage_lp.cpp`` skips the drain column.
        if res.spill_penalty_per_mwh > 0.0:
            # gtopt ``spillway_cost`` is per-(m³/s)/h, PLEXOS reports
            # per-MWh — multiply by the global default productibility
            # (DESIGN.md §6).
            entry["spillway_cost"] = res.spill_penalty_per_mwh * DEFAULT_FP_MED
        else:
            # When ``GTOPT_RESERVOIR_SPILL=basic`` or ``strict`` (the
            # ``--reservoir-spillway`` CLI flag), activate the
            # reservoir-internal spillway with COST = 0.  Mirrors
            # PLEXOS's implicit Storage-state spillage: when inflow
            # exceeds the LP's downstream room (capped turbine +
            # capped cascade exit), water "disappears" via this
            # internal drain at no cost.  Mode semantics differ in
            # extract_case where the duplicate-mechanism cleanup runs.
            import os as _os

            mode = _os.environ.get("GTOPT_RESERVOIR_SPILL", "").lower()
            if mode in ("1", "true", "yes", "basic", "strict"):
                entry["spillway_cost"] = 0.0
        # Default annual evaporation / seepage loss for hydroelectric
        # reservoirs — gtopt's ``Reservoir.annual_loss`` (p.u./year
        # linear) drives the energy-balance row coefficient
        # ``V[t+1] = V[t] × (1 − annual_loss / 8760 × duration) + flows``.
        #
        # Literature on reservoir evaporation losses (annual fraction
        # of usable storage lost to surface evaporation + seepage):
        #   * ICOLD Bulletin on Reservoir Operation: 3–5 %/year average
        #     worldwide
        #   * World Bank Hydropower Sustainability Assessment Protocol:
        #     0.5–10 %/year, climate-dependent
        #   * Andean / cool-climate reservoirs (CEN: PEHUENCHE, RALCO,
        #     COLBUN, ELTORO, MACHICURA, PANGUE — all alpine):
        #     1–3 %/year typical
        #   * Mediterranean / arid reservoirs: 4–7 %/year
        #   * Tropical / desert reservoirs: 8–15 %/year
        #
        # 4 %/year is a reasonable mid-range default for CEN's
        # Andean reservoirs — slightly above the cool-climate 1–3 %
        # band to account for the high-altitude UV-driven evaporation
        # at the larger surface-area reservoirs (RAPEL, COLBUN).
        # Conservative enough not to distort the LP economics but
        # large enough to gently anchor the storage chain and
        # regularise the LP basis.
        #
        # Only emit when PLEXOS doesn't already ship its own
        # ``annual_loss`` value (which the converter pulls from
        # ``Storage.Annual Loss Rate`` if PLEXOS sets it — never
        # observed populated in v0407 but the safety check costs
        # nothing).  Skip reservoirs with effectively zero storage
        # (pass-through / RoR pondage where the loss is meaningless).
        if "annual_loss" not in entry and res.emax > 0.0:
            entry["annual_loss"] = 0.04  # 4 %/year — CEN Andean default
        out.append(entry)
    return out


def build_junction_array(
    junctions: tuple[JunctionSpec, ...],
) -> list[dict[str, Any]]:
    """One Junction per :class:`JunctionSpec`.

    Emits the new ``Junction.drain_capacity`` and ``Junction.drain_cost``
    fields when the JunctionSpec carries values for them (set by
    ``extract_waterways`` when collapsing a ``Vert_<src>`` spillway onto
    the source storage's own junction).  Both are omitted when ``None``
    so gtopt's LP-side defaults (``DblMax`` / ``0.0``) apply.
    """
    out: list[dict[str, Any]] = []
    for i, j in enumerate(junctions):
        entry: dict[str, Any] = {"uid": i + 1, "name": j.name}
        if j.drain:
            entry["drain"] = True
        if j.drain_capacity is not None:
            entry["drain_capacity"] = j.drain_capacity
        if j.drain_cost is not None:
            entry["drain_cost"] = j.drain_cost
        out.append(entry)
    return out


def build_waterway_array(
    waterways: tuple[WaterwaySpec, ...],
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One Waterway per :class:`WaterwaySpec` with both endpoints valid.

    PLEXOS Storage From / Storage To names map directly to gtopt's
    ``junction_a`` / ``junction_b`` since our junction naming mirrors
    the source Storage names.

    When ``ww.forced_flow_profile`` carries a non-constant per-hour
    series, both ``fmin`` and ``fmax`` are emitted as per-block
    matrices (``[[v_b0, v_b1, ...]]``) computed by aggregating the
    hourly profile onto the active ``block_layout`` (mean reducer,
    preserving units and the constant-pin semantics of the original
    PLEXOS forced flow).  Without this matrix path, pinning
    ``fmin = fmax = max(profile)`` uniformly produced phantom water
    at B_Maule on the CEN PCP daily bundle — see the docstring on
    :class:`WaterwaySpec.forced_flow_profile` for the diagnosis.
    """
    out: list[dict[str, Any]] = []
    for i, ww in enumerate(waterways):
        if ww.storage_from is None or ww.storage_to is None:
            continue
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": ww.name,
            "junction_a": ww.storage_from,
            "junction_b": ww.storage_to,
        }
        profile = ww.forced_flow_profile
        if profile and min(profile) != max(profile):
            per_block = (
                _aggregate_to_blocks(profile, block_layout, reducer="mean")
                if block_layout
                else list(profile)
            )
            entry["fmin"] = [per_block]
            # Bypass waterways (e.g. ``B_Maule``): emit profile as
            # fmin only and leave fmax unbounded so the LP can route
            # surplus above the PLEXOS Min Flow.  Diversion / sink
            # waterways (Riego_, Caudal_Eco_, Filt_) keep fmin == fmax.
            if ww.pin_fmax_from_profile:
                entry["fmax"] = [per_block]
        else:
            if ww.fmax > 0.0:
                entry["fmax"] = ww.fmax
            if ww.fmin > 0.0:
                entry["fmin"] = ww.fmin
        if ww.fcost > 0.0:
            entry["fcost"] = ww.fcost
        out.append(entry)
    return out


def build_turbine_array(
    turbines: tuple[TurbineSpec, ...],
    waterways: tuple[WaterwaySpec, ...] = (),
    extra_waterways: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    """One Turbine per :class:`TurbineSpec`.

    Each entry links a Generator (electrical output) to its upstream
    Reservoir (water balance via ``main_reservoir``).

    **Built-in waterway mode (build_planning call site).** gtopt's
    Turbine carries its own flow arc: setting ``junction_a`` (the
    reservoir's intake junction) and ``junction_b`` (the downstream
    junction) makes the turbine debit ``junction_a`` and credit
    ``junction_b`` exactly like a Waterway, *and* convert the carried
    flow to power (``gen = pf × flow``).  This replaces the previous
    approach of synthesising one zero-cost penstock Waterway per
    turbine — the turbine **is** its own penstock now, so no per-unit
    Waterway clones and no ``<reservoir>_terminal_ocean`` Junctions are
    emitted.  Multi-unit plants (ANTUCO U1/U2, MACHICURA U1/U2,
    PEHUENCHE U1/U2, …) each get an independent flow column, so the
    per-unit ``gen = pf × flow`` equalities never force ``gen_u1 =
    gen_u2``.

    PLEXOS ``Vert_*`` Waterways remain *spillways* (vertimiento —
    bypass water priced with a per-flow penalty); turbines no longer
    route through them.  The original PLEXOS Waterways are left
    untouched and continue to model physical spillage independently.

    Terminal hydro plants (LAJA_I, ANGOSTURA, CANUTILLAR, RAPEL on
    CEN PCP) have no downstream junction: the turbine is emitted with
    ``junction_a`` only and ``junction_b`` unset, so the turbined flow
    drains out of the modelled system (run-to-sea) without a
    synthesised ocean junction.  The C++ ``TurbineLP`` skips the flow
    arc for any block whose generator pmax is zero, so a unit can never
    push water it could not physically discharge — the old penstock
    ``fmax = pmax_peak / pf`` cap is no longer needed.

    **Legacy mode (no ``extra_waterways``, e.g. unit tests).** Falls
    back to linking the turbine to an existing PLEXOS Waterway via
    ``waterway``; turbines with no such link are dropped with a summary
    log line.
    """
    ww_by_from: dict[str, str] = {}
    ww_by_name: dict[str, WaterwaySpec] = {}
    for w in waterways:
        ww_by_name[w.name] = w
        if w.storage_from and w.storage_from not in ww_by_from:
            ww_by_from[w.storage_from] = w.name

    out: list[dict[str, Any]] = []
    skipped: list[str] = []
    builtin_count = 0
    terminal_count = 0
    for t in turbines:
        # Prefer PLEXOS Tail Storage when shipped: that's the canonical
        # downstream junction the turbine discharges into, which may
        # differ from the spillway's tail (e.g. a turbine may discharge
        # into a regulation pond while the Vert_* spillway routes to a
        # different downstream junction).  Only fall back to the
        # spillway's storage_to when Tail Storage is absent.
        downstream: str | None = t.tail_reservoir_name
        if downstream is None or downstream == t.reservoir_name:
            downstream_ww_name = ww_by_from.get(t.reservoir_name)
            if downstream_ww_name is not None:
                original = ww_by_name.get(downstream_ww_name)
                if original is not None and original.storage_to is not None:
                    downstream = original.storage_to

        entry: dict[str, Any] = {
            "uid": len(out) + 1,
            "name": f"turbine_{t.generator_name}",
            "generator": t.generator_name,
            "main_reservoir": t.reservoir_name,
        }
        if extra_waterways is not None:
            # Built-in waterway mode: the turbine carries its own flow
            # arc.  ``junction_a`` is the reservoir's intake junction;
            # ``junction_b`` (when present) is the downstream junction.
            entry["junction_a"] = t.reservoir_name
            if downstream is not None and downstream != t.reservoir_name:
                entry["junction_b"] = downstream
            else:
                # Terminal (run-to-sea) plant: no downstream junction —
                # the turbined flow drains out of the system.  No ocean
                # junction is synthesised.
                terminal_count += 1
            builtin_count += 1
        else:
            # Legacy mode: link to an existing PLEXOS waterway.
            waterway_ref = ww_by_from.get(t.reservoir_name) or ""
            if not waterway_ref:
                skipped.append(t.generator_name)
                continue
            entry["waterway"] = waterway_ref
        if t.production_factor > 0.0:
            entry["production_factor"] = t.production_factor
        out.append(entry)
    if builtin_count:
        logger.info(
            "build_turbine_array: emitted %d turbines as built-in waterways "
            "(junction_a/junction_b flow arcs, replacing the per-unit penstock "
            "waterways), of which %d are terminal (junction_b unset → drained, "
            "no synthesised ocean junction).",
            builtin_count,
            terminal_count,
        )
    if skipped:
        logger.info(
            "build_turbine_array: dropped %d turbines with no Waterway link "
            "(terminal hydro plants without explicit downstream spillway + "
            "thermal pseudo-turbines on gas-storage Storage objects). "
            "Sample: %s",
            len(skipped),
            ", ".join(skipped[:5]),
        )
    return out


def build_flow_array(
    flows: tuple[FlowSpec, ...],
    *,
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One Flow per :class:`FlowSpec`, broadcasting the per-block profile.

    gtopt's ``Flow.discharge`` is a per-(scene, stage, block) shape.
    For the daily PCP horizon we emit a single (1×1×N) matrix.

    Under ``--horizon-mode plexos`` the hourly inflow profile is
    aggregated to one value per block (mean over the block's hourly
    intervals) so the matrix lines up with the block_array.
    """
    out: list[dict[str, Any]] = []
    for i, f in enumerate(flows):
        profile = (
            _aggregate_to_blocks(f.discharge_profile, block_layout, reducer="mean")
            if block_layout
            else list(f.discharge_profile)
        )
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": f.name,
            "junction": f.junction_name,
        }
        # Non-physical inflow slacks (``fcost > 0``) carry NO
        # ``discharge`` at all — gtopt's optional ``Flow.discharge``
        # defaults the column upper bound to ``+inf`` (``DblMax``),
        # which is exactly what we want for a costed slack the LP
        # only activates as needed.  Regular natural inflows keep
        # the per-block profile so the forced equality
        # ``flow = discharge`` reflects the actual hourly inflow.
        if f.fcost > 0.0:
            entry["fcost"] = f.fcost
        else:
            entry["discharge"] = [[profile]]
        out.append(entry)
    return out


def build_reserve_zone_array(
    reserves: tuple[ReserveSpec, ...],
    block_count: int = DEFAULT_BLOCK_COUNT,
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One ``ReserveZone`` per :class:`ReserveSpec` with a populated profile.

    ``urreq`` / ``drreq`` are emitted as ``[[stage_blocks]]`` matrices
    (1 stage × N blocks). Zones without a profile get a zero-vector
    requirement so gtopt unconditionally materialises the up/down
    provision columns — matching PLEXOS semantics where the reserve-
    provision variable exists for every (Reserve, Generator)
    eligibility regardless of the zone's Risk/Requirement. Without
    the zero-vector default, PLEXOS Constraints with non-trivial
    Reserve Provision Coefficients (CEN PCP CFRS_* / CFRR_* etc.)
    would crash LP assembly when the zone has no system-level
    requirement.

    ``block_count`` controls the per-stage profile length (24 for the
    legacy 1-day case, 24*``n_days`` for hourly multi-day, or
    ``len(block_layout)`` for PLEXOS-native aggregated runs).  When a
    non-empty ``block_layout`` is supplied, hourly profiles are
    aggregated to one value per layout block using the mean reducer
    (requirements are MW levels, not energies, so averaging the hours
    inside the block matches the gtopt block-level constraint).
    """
    target_len = len(block_layout) if block_layout else block_count
    zero_profile = [0.0] * target_len
    out: list[dict[str, Any]] = []
    for i, rsv in enumerate(reserves):
        entry: dict[str, Any] = {"uid": i + 1, "name": rsv.name}

        def _shape(profile: tuple[float, ...]) -> list[float]:
            if not profile:
                return zero_profile
            hourly = list(profile)
            if block_layout:
                return _aggregate_to_blocks(hourly, block_layout, reducer="mean")
            if len(hourly) == target_len:
                return hourly
            # Tile (single-day pattern repeats) or truncate to fit.
            if len(hourly) > 0 and target_len % len(hourly) == 0:
                return hourly * (target_len // len(hourly))
            # Last resort: pad with zeros so the LP never reads past
            # the array end.
            padded = hourly[:target_len]
            padded.extend([0.0] * (target_len - len(padded)))
            return padded

        entry["urreq"] = [_shape(rsv.ur_requirement)]
        entry["drreq"] = [_shape(rsv.dr_requirement)]
        # Shortage-penalty costs ($/MWh) emit only when non-zero — gtopt
        # treats the unset OptTBRealFieldSched as "no penalty", matching
        # PLEXOS semantics for Reserves that ship VoRS=-1 (sentinel).
        if rsv.urcost > 0.0:
            entry["urcost"] = rsv.urcost
        if rsv.drcost > 0.0:
            entry["drcost"] = rsv.drcost
        out.append(entry)
    return out


def build_decision_variable_array(
    decision_variables: tuple[DecisionVariableSpec, ...],
) -> list[dict[str, Any]]:
    """One ``DecisionVariable`` per :class:`DecisionVariableSpec`.

    Bounds emit only when set on the spec (``None`` leaves the LP
    column free in that direction); cost emits only when non-zero.

    No ``cost_type`` is emitted: the C++ default is ``"raw"`` (face-value
    $, NOT probability/discount/duration-weighted), which is correct for the
    general PLEXOS DecisionVariables (penalties, reserve VoRS, BESS knobs —
    discrete face-value costs).  Δt-weighting them (the old "power" default)
    over-charged them by the block length.  ``alpha_fcf`` is built
    separately and sets ``cost_type: "raw"`` explicitly (so it is
    unaffected by this default).
    """
    out: list[dict[str, Any]] = []
    for i, dv in enumerate(decision_variables):
        entry: dict[str, Any] = {"uid": i + 1, "name": dv.name}
        if dv.lower_bound is not None:
            entry["lower_bound"] = dv.lower_bound
        if dv.upper_bound is not None:
            entry["upper_bound"] = dv.upper_bound
        if dv.cost != 0.0:
            entry["cost"] = dv.cost
        out.append(entry)
    return out


def build_plant_array(
    plants: tuple[PlantSpec, ...],
) -> list[dict[str, Any]]:
    """One ``Plant`` per :class:`PlantSpec`.

    Native gtopt primitive that replaces the synthesised
    ``PlantCap_<stem>`` UserConstraints and ``<plant>_Uniq`` mutex UCs
    emitted by earlier converter versions.  ``PlantLP`` enforces a
    hard ``Σ generation ≤ pmax`` row per stage × block, plus optional
    ``Σ commit·status ≤ n_units`` / ``Σ status ≤ 1`` rows.

    Only fields that the spec carries are emitted — ``n_units``,
    ``commit_coeffs``, and ``uniq_mutex`` are skipped when at their
    no-op defaults.
    """
    out: list[dict[str, Any]] = []
    for i, p in enumerate(plants):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": p.name,
            "generator_names": list(p.generator_names),
        }
        if p.pmax is not None:
            entry["pmax"] = p.pmax
        if p.n_units is not None:
            entry["n_units"] = p.n_units
        if p.commit_coeffs:
            entry["commit_coeffs"] = list(p.commit_coeffs)
        if p.uniq_mutex:
            entry["uniq_mutex"] = True
        out.append(entry)
    return out


def build_user_constraint_array(
    constraints: tuple[UserConstraintSpec, ...],
    *,
    default_penalty: float | None = None,
    block_count: int = DEFAULT_BLOCK_COUNT,
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One ``UserConstraint`` per :class:`UserConstraintSpec`.

    ``penalty > 0`` flips the row into a soft constraint with an
    auto-created slack column (see ``user_constraint.hpp`` for the
    LP-side semantics).

    ``default_penalty`` is a diagnostic fallback applied to every UC
    whose PLEXOS source has no explicit Penalty Price.  CEN PCP ships
    every Constraint as hard (no penalty), so without this knob a
    single unsatisfiable row makes the whole LP infeasible.  Setting
    e.g. ``--default-uc-penalty 10000`` keeps the LP feasible and
    surfaces per-constraint slack violations in the solver output.

    When a spec's ``rhs_profile`` is non-empty, it is shaped to the
    horizon's per-stage block count (using ``block_layout`` for
    PLEXOS-native aggregation when supplied) and emitted as the
    gtopt ``user_constraint.rhs`` TB-schedule field, overriding the
    scalar parsed from the inline ``<op> NUMBER`` tail of
    ``expression`` at every (stage, block).
    """
    target_len = len(block_layout) if block_layout else block_count

    def _shape_profile(profile: tuple[float, ...]) -> list[float]:
        hourly = list(profile)
        # Already per-block (matches the target layout)?  Pass through.
        # Consolidators like ``_consolidate_gas_maxopday_groups`` emit a
        # profile of length ``len(block_layout)`` directly when block_layout
        # is supplied; aggregating it again via ``_aggregate_to_blocks``
        # would misinterpret it as hourly (intervals 1..168) and zero out
        # every block whose ``max(intervals) > len(profile)``.  Verified on
        # CEN PCP 2026-04-07 ``Gas_MaxOpDay_NuevaRenca``: 33 trailing
        # blocks (intervals 113..168) were silently zeroed, causing the
        # LP to slack $1.62 M of NuevaRenca gas dispatch PLEXOS itself
        # never restricts.
        if len(hourly) == target_len:
            return hourly
        if block_layout:
            return _aggregate_to_blocks(hourly, block_layout, reducer="mean")
        if len(hourly) > 0 and target_len % len(hourly) == 0:
            return hourly * (target_len // len(hourly))
        padded = hourly[:target_len]
        padded.extend([0.0] * (target_len - len(padded)))
        return padded

    out: list[dict[str, Any]] = []
    for i, c in enumerate(constraints):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": c.name,
            "expression": c.expression,
        }
        penalty = c.penalty
        if penalty <= 0.0 < (default_penalty or 0.0):
            penalty = default_penalty  # type: ignore[assignment]
        if penalty > 0.0:
            entry["penalty"] = penalty
        if c.description:
            entry["description"] = c.description
        if c.active is not None:
            entry["active"] = bool(c.active)
        if c.rhs_profile:
            entry["rhs"] = [_shape_profile(c.rhs_profile)]
        # Daily-ENERGY budget (PLEXOS ``RHS Day`` / ramp-day): gtopt's
        # ``daily_sum`` collapses the per-block expansion to one LP row per
        # 24 h day; ``constraint_type=energy`` Δt-weights each block so the
        # LHS is ``Σ_day gen·Δt`` [MWh].  Routed to the inline JSON
        # ``user_constraint_array`` (the ``.pampl`` grammar has no
        # ``daily_sum`` clause — see ``write_user_constraint_pampl``).
        #
        # Emit ``constraint_type`` BEFORE ``daily_sum`` to match the C++
        # ``json_data_contract<UserConstraint>`` schema order
        # (``include/gtopt/json/json_user_constraint.hpp`` lines 80, 83).
        # daw::json's StrictParsePolicy enforces the schema order — when
        # both fields plus ``slack_name`` (pos 12) co-occur and
        # ``daily_sum`` (pos 9) precedes ``constraint_type`` (pos 6) in
        # JSON, the parser rejects the trailing ``slack_name`` with
        # "Could not find member in JSON class".  Verified on the
        # 2026-04-07 PCP bundle's ``PANGUEcaudal_min_diario`` UC.
        if c.constraint_type:
            entry["constraint_type"] = c.constraint_type
        if c.daily_sum:
            entry["daily_sum"] = True
        if c.directive is not None:
            # Typed constraint-family metadata — replaces the legacy
            # name-regex / penalty-ladder classification with an
            # auditable, schema-validated sibling field that the
            # gtopt-side ``UserConstraint::directive`` picks up.
            # Only emit the keys actually populated on the directive
            # so the JSON stays minimal (gtopt's daw::json contract
            # treats every directive field as ``*_null`` / optional).
            #
            # See ``include/gtopt/constraint_directive.hpp`` for the
            # discriminator → payload schema and AMPL/PAMPL
            # modernization plan (2026-05-30) Step 4a for the migration
            # rationale (#53).
            directive_entry: dict[str, Any] = {"kind": c.directive.kind}
            if c.directive.penalty is not None:
                directive_entry["penalty"] = c.directive.penalty
            if c.directive.scope:
                directive_entry["scope"] = c.directive.scope
            if c.directive.window_hours is not None:
                directive_entry["window_hours"] = c.directive.window_hours
            entry["directive"] = directive_entry
        # Visible-slack column label: when this UC is soft, set
        # ``slack_name`` so the gtopt-side ``UserConstraintLP`` uses
        # the per-UC label (``slack_<sanitised>``) for the auto-created
        # slack column.  Keeps the inline-JSON path aligned with the
        # PAMPL path, where the parser binds slacks via the matching
        # ``var slack_<ident>;`` declaration (the JSON path has no
        # ``var`` statement so we set the field directly).  The ident
        # uses the same sanitisation rule the PAMPL writer applies so
        # downstream tooling can match constraints to slacks the same
        # way regardless of which emission path produced them.
        if penalty > 0.0:
            entry["slack_name"] = f"slack_{_pampl_ident(c.name)}"
        out.append(entry)
    return out


def build_flow_right_array(
    flow_rights: tuple[FlowRightSpec, ...],
    block_count: int = DEFAULT_BLOCK_COUNT,
    block_layout: tuple[tuple[int, ...], ...] = (),
    extra_waterways: list[dict[str, Any]] | None = None,
    waterways: tuple[WaterwaySpec, ...] = (),
) -> list[dict[str, Any]]:
    """One ``FlowRight`` per :class:`FlowRightSpec` with a resolved junction.

    Specs whose ``junction_name`` is None are dropped — gtopt's
    FlowRight requires a junction reference to apply. The drop is
    logged once when the parser resolves them, so the writer stays
    quiet.

    ``block_count`` / ``block_layout`` define the per-stage profile
    length so the broadcast ``fcost`` row matches the simulation
    horizon.  Without this gtopt's ``FieldSched`` lookup would read
    past the end of a 24-element row on multi-day runs.

    **Inline bypass via FlowRight.bypass_junction**: gtopt's
    ``FlowRight`` LP class supports a pass-through column priced at
    ``bypass_cost·cf`` that contributes -1 to ``junction`` and +1 to
    ``bypass_junction`` (see ``flow_right_lp.cpp``).  When a
    ``FlowRightSpec`` declares ``bypass_junction``, the writer emits
    that on the JSON entry — the LP layer adds the pressure-release
    flow inline rather than via a parallel synthetic Waterway.  When
    no ``bypass_junction`` is set on the spec, we auto-resolve one
    from the topology: the first existing ``Vert_*`` spillway's
    downstream from the FlowRight's junction.  This preserves the
    pre-feature pressure-release semantics without polluting the
    Waterway array with synthetic ``bypass_*`` rows.
    """
    target_len = len(block_layout) if block_layout else block_count
    # Map: junction → first downstream of any existing Vert_* waterway
    spill_downstream: dict[str, str] = {}
    for w in waterways:
        if w.storage_from and w.storage_to and w.name.startswith("Vert_"):
            if w.storage_from not in spill_downstream:
                spill_downstream[w.storage_from] = w.storage_to
    out: list[dict[str, Any]] = []
    bypassed = 0
    _ = extra_waterways  # kept for API compatibility; no longer mutated
    for i, fr in enumerate(flow_rights):
        if fr.junction_name is None:
            continue
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": fr.name,
            "junction": fr.junction_name,
            "purpose": fr.purpose,
        }
        if fr.fmin > 0.0:
            entry["fmin"] = fr.fmin
        if fr.fmax > 0.0:
            entry["fmax"] = fr.fmax
        # Soft kink point — only meaningful when paired with fcost
        # and/or uvalue.  Required for the LP-side ``fail_col`` /
        # ``excess_col`` slack machinery to activate.
        if fr.target > 0.0:
            entry["target"] = fr.target
        # fcost is OptTBRealFieldSched (same shape as Demand.fcost):
        # broadcast the scalar across the horizon as a [[stage_blocks]]
        # matrix when set.
        if fr.fcost > 0.0:
            entry["fcost"] = [[fr.fcost] * target_len]
        # Inline bypass: explicit override wins, otherwise auto-resolve
        # from existing Vert_* topology.  bypass_cost = 0 is the
        # legacy default (free pass-through, used only when the cap
        # binds), preserving prior behaviour.
        bypass_to = fr.bypass_junction or spill_downstream.get(fr.junction_name)
        if bypass_to is not None:
            entry["bypass_junction"] = bypass_to
            if fr.bypass_cost > 0.0:
                entry["bypass_cost"] = fr.bypass_cost
            bypassed += 1
        out.append(entry)
    if bypassed:
        logger.info(
            "build_flow_right_array: emitted %d FlowRight(s) with "
            "inline bypass_junction (pressure-release via FlowRight LP "
            "instead of synthetic parallel Waterway).",
            bypassed,
        )
    return out


def build_commitment_array(
    commitments: tuple[CommitmentSpec, ...],
    *,
    lp_relax: bool = False,
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One ``Commitment`` per :class:`CommitmentSpec`.

    ``relax: true`` is emitted only when ``lp_relax=True`` (CLI:
    ``--lp-relax``).  Default is MIP — commitments ship without the
    ``relax`` field so gtopt enforces binary integrality on the
    status / startup / shutdown variables.

    The default flipped from LP-relax to MIP on 2026-05-23 after
    diagnosing the CEN PCP PLEXOS reproduction: with LP-relax the
    ``<plant>_Uniq`` constraints (``Σ status ≤ 1`` over band
    variants) collapsed to fractional commitments, letting the LP
    spread dispatch across band variants and undercut PLEXOS by ~31%
    on operational cost.  MIP enforcement closed ~7 pp of that gap
    and moved NUEVA_RENCA-TG+TV_GN_A dispatch from 19 GWh/week (LP)
    to 33 GWh/week (MIP) vs PLEXOS's 40 GWh.

    Pass ``--lp-relax`` for the legacy LP-only behaviour (faster
    solve, less accurate dispatch) or for solvers without MIP
    support (CLP, OSI without CBC, etc.).
    """
    out: list[dict[str, Any]] = []
    for i, c in enumerate(commitments):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": f"uc_{c.generator_name}",
            "generator": c.generator_name,
        }
        if lp_relax:
            entry["relax"] = True
        if c.startup_cost > 0.0:
            entry["startup_cost"] = c.startup_cost
        if c.shutdown_cost > 0.0:
            entry["shutdown_cost"] = c.shutdown_cost
        if c.min_up_time > 0.0:
            entry["min_up_time"] = c.min_up_time
        if c.min_down_time > 0.0:
            entry["min_down_time"] = c.min_down_time
        # Ramp limits: per-block CPF curve (``[[block values]]``) when it
        # varies intra-horizon, else the scalar.  Mirrors ``pmin`` above —
        # gtopt ``Commitment.ramp_up/down`` is now a TB schedule.
        if c.ramp_up_profile and (max(c.ramp_up_profile) != min(c.ramp_up_profile)):
            entry["ramp_up"] = [
                _aggregate_to_blocks(c.ramp_up_profile, block_layout, reducer="mean")
                if block_layout
                else list(c.ramp_up_profile)
            ]
        elif c.ramp_up > 0.0:
            entry["ramp_up"] = c.ramp_up
        if c.ramp_down_profile and (
            max(c.ramp_down_profile) != min(c.ramp_down_profile)
        ):
            entry["ramp_down"] = [
                _aggregate_to_blocks(c.ramp_down_profile, block_layout, reducer="mean")
                if block_layout
                else list(c.ramp_down_profile)
            ]
        elif c.ramp_down > 0.0:
            entry["ramp_down"] = c.ramp_down
        if c.startup_ramp > 0.0:
            entry["startup_ramp"] = c.startup_ramp
        # initial_status / initial_hours: emit even when zero so the
        # LP knows the unit was offline at t=0.
        entry["initial_status"] = c.initial_status
        if c.initial_hours != 0.0:
            entry["initial_hours"] = c.initial_hours
        if c.noload_cost > 0.0:
            entry["noload_cost"] = c.noload_cost
        # commitment.pmin: per-unit Min Stable Level when committed —
        # gtopt's commitment_lp.cpp reads this directly (when set)
        # and skips resetting Generator.pmin.  Distinct from
        # Generator.pmin (always-on floor).  CEN PCP uses Min Stable
        # Level here; Generator.pmin stays 0 (no always-on floor).
        #
        # PLEXOS ships Min Stable Level as a time series.  When it
        # varies across the horizon, emit the per-block schedule
        # (aggregated to the block_array layout, ``[[block values]]``
        # = one stage) so gtopt backs the unit down in its low-floor
        # periods instead of pinning the period-1 value all week
        # (e.g. SANTA_MARIA: 98.53 MW for 148 h, 170.53 MW for 20 h).
        # Constant profiles collapse to the scalar.
        if c.pmin_profile and (max(c.pmin_profile) != min(c.pmin_profile)):
            pmin_blocks = (
                _aggregate_to_blocks(c.pmin_profile, block_layout, reducer="mean")
                if block_layout
                else list(c.pmin_profile)
            )
            entry["pmin"] = [pmin_blocks]
        elif c.pmin > 0.0:
            entry["pmin"] = c.pmin
        # PLEXOS ``Generator.Commit`` forcing (Gen_Commit profile) →
        # pin gtopt's ``u`` (commitment status) so the LP honours
        # PLEXOS's must-run / don't-commit decisions THROUGH THE
        # COMMITMENT VARIABLE — ``pmax`` is left untouched.
        #   * every period +1  → ``must_run: true`` (u = 1 all stage).
        #   * any forced period → per-block ``fixed_status`` (1.0 pins
        #     u=1, 0.0 pins u=0, -1.0 leaves the block free).
        #   * all -1 (endogenous) → nothing (gtopt decides).
        prof = c.commit_status_profile
        if prof:
            if all(v == 1 for v in prof):
                entry["must_run"] = True
            elif any(v in (0, 1) for v in prof):
                fixed = _aggregate_commit_status(prof, block_layout)
                if any(v >= 0.0 for v in fixed):  # at least one pinned block
                    entry["fixed_status"] = [fixed]
        # PLEXOS ``Initial Generation`` (Gen_IniGeneration.csv) →
        # gtopt ``Commitment.initial_power``: the dispatch level at
        # t = -1 used by the first-block ramp / commitment continuity
        # rows.  Emit only when non-zero so the JSON stays clean for
        # cold-start units (the gtopt LP defaults to ``p_prev = 0``
        # when the field is unset — correct for genuine cold starts).
        if c.initial_power != 0.0:
            entry["initial_power"] = c.initial_power
        # PLEXOS ``Max Starts {Hour|Day|Week|...}`` → gtopt's two-sided
        # ``Commitment.{min_starts, max_starts}`` + ``starts_scope``.
        # PLEXOS only ships the cap side (Max Starts family); the
        # symmetric ``min_starts`` floor is left at 0 here but gtopt's
        # LP-side primitive supports both bounds simultaneously
        # (``CommitmentLP::add_to_lp`` C9: ``min_starts ≤ Σ_{p ∈ window}
        # v[p] ≤ max_starts``).  Unset on either side → that side's row
        # is not emitted (max unset = +∞, min unset = 0).  Emission
        # requires AT LEAST one side > 0 plus a non-empty scope.
        if (c.max_starts > 0 or c.min_starts > 0) and c.starts_scope:
            if c.max_starts > 0:
                entry["max_starts"] = int(c.max_starts)
            if c.min_starts > 0:
                entry["min_starts"] = int(c.min_starts)
            entry["starts_scope"] = str(c.starts_scope)
        out.append(entry)
    return out


def build_reserve_provision_array(
    provisions: tuple[ReserveProvisionSpec, ...],
    block_layout: tuple[tuple[int, ...], ...] = (),
) -> list[dict[str, Any]]:
    """One ``ReserveProvision`` per :class:`ReserveProvisionSpec`.

    ``ur_provision_factor`` / ``dr_provision_factor`` default to 1.0
    and ``ur_capacity_factor`` / ``dr_capacity_factor`` to 1.0 so
    gtopt's :file:`reserve_provision_lp.cpp` unconditionally creates
    the per-block ``up`` / ``dn`` columns. PLEXOS forces reserve
    provision via Constraint coefficients regardless of zone-level
    Risk/Requirement; without this default the LP would skip the
    columns and any user constraint referencing
    ``reserve_provision(...).up`` / ``.dn`` would dangle.
    """

    def _factor(profile: tuple[float, ...]) -> list[list[float]] | float:
        if not profile:
            return 1.0
        blocks = (
            _aggregate_to_blocks(list(profile), block_layout, reducer="mean")
            if block_layout
            else list(profile)
        )
        return [blocks]

    out: list[dict[str, Any]] = []
    for i, p in enumerate(provisions):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": p.name or f"provision_{p.generator_name}",
            "generator": p.generator_name,
            "reserve_zones": list(p.reserve_zones),
            # Per-block up/down provision factor (SSCC BESS activation
            # schedule) when supplied; else the scalar 1.0 default that
            # keeps the LP column unconditionally materialised.
            "ur_provision_factor": _factor(p.ur_provision_factor_profile),
            "dr_provision_factor": _factor(p.dr_provision_factor_profile),
        }
        # Per-block urmax / drmax profile from CEN's CFdata/{CPF,CSF,CTF}
        # MRU/MRD files (the AUTHORITATIVE per-(gen, reserve, hour) MAX
        # RESERVE CAPABILITY in MW).  When populated, takes precedence
        # over the scalar ``urmax = pmax`` fallback because the CFdata
        # cap is 2-16000× tighter than ``pmax`` — verified 2026-05-31
        # on v0407 PLEXOS sol: the per-hour cap == PLEXOS sol binding
        # provision exactly for every (gen, reserve) pair.  Without
        # this, gtopt's ``reserve_provision.up / .dn`` columns sit
        # unbounded above and the PLEXOS reserve UCs
        # (CPF_Up5Calculation -$84, CSF_UpMinProvision -$43) never
        # bind; also triggers the UPStorageBound_BAT_* -inf dual
        # cascade.  Aggregator: SUM of MRU across CPF/CSF/CTF (CTFON
        # tertiary on-line) per direction — the conservative upper
        # envelope for the single-column reserve_provision LP variable.
        if p.urmax_profile:
            blocks = (
                _aggregate_to_blocks(list(p.urmax_profile), block_layout, reducer="min")
                if block_layout
                else list(p.urmax_profile)
            )
            entry["urmax"] = [blocks]
        elif p.urmax > 0.0:
            entry["urmax"] = p.urmax
        if p.drmax_profile:
            blocks = (
                _aggregate_to_blocks(list(p.drmax_profile), block_layout, reducer="min")
                if block_layout
                else list(p.drmax_profile)
            )
            entry["drmax"] = [blocks]
        elif p.drmax > 0.0:
            entry["drmax"] = p.drmax
        # PLEXOS "Min Provision" is a hard floor on reserve provision
        # that PLEXOS itself gates on the generator being COMMITTED in
        # the block.  gtopt's ``reserve_provision_lp.cpp`` does NOT
        # apply such gating; setting ``urmin``/``drmin`` ships them as
        # ``provision >= urmin`` for every block, regardless of unit
        # status.  When the LP wants the unit dispatched below ``urmin``
        # (or off entirely while the unit is "available"), the floor
        # collides with ``provision <= gen`` and the LP becomes
        # infeasible (observed on EL_TORO_U4 block 32 in CEN PCP 7d).
        # Drop ``urmin``/``drmin`` until gtopt supports commitment-
        # conditional reserve provision floors — PLEXOS Min Provision
        # is currently unmodeled.  Reserve provision stays in
        # ``[0, urmax]`` / ``[0, drmax]``.
        _ = p.urmin
        _ = p.drmin
        out.append(entry)
    return out


def _collapse_orphan_drain_outflows(system: dict[str, Any]) -> int:
    """Convert waterway / turbine `junction_b` refs that target an orphan
    drain-only sink junction to **outflow mode** (drop ``junction_b``) and
    remove the now-unreferenced junction.

    A junction qualifies when:
      * ``drain=True`` AND
      * name ends in ``_sink`` or ``_ocean`` (the converter's drain-only
        synthesis convention — distinguishes from real reservoir junctions
        that may also carry ``drain=True`` from spillway collapse) AND
      * referenced ONLY as ``junction_b`` of waterway / turbine entries
        (no other consumer needs it).

    Saves one synthetic junction per spillway / diversion outflow while
    keeping the flow VISIBLE on its waterway / turbine (cf. PLEXOS Vert_*
    pattern: the spillage stays as a flow column, no extra sink junction).

    Returns the number of junctions collapsed.
    """
    juncs = system.get("junction_array", [])
    waterways = system.get("waterway_array", [])
    turbines = system.get("turbine_array", [])

    def _is_candidate(j: dict[str, Any]) -> bool:
        if not j.get("drain"):
            return False
        name = str(j.get("name", ""))
        return name.endswith("_sink") or name.endswith("_ocean")

    cand_names: set[str] = {j["name"] for j in juncs if _is_candidate(j)}
    if not cand_names:
        return 0

    by_uid: dict[int, str] = {j["uid"]: j["name"] for j in juncs}

    def _ref_name(ref: Any) -> str | None:
        if isinstance(ref, str):
            return ref
        if isinstance(ref, int):
            return by_uid.get(ref)
        return None

    # (kind, element) for outflow-convertible refs; a sentinel ("block",
    # None) marks any OTHER consumer that pins the junction in place.
    BLOCK: tuple[str, Any] = ("block", None)
    refs: dict[str, list[tuple[str, Any]]] = {n: [] for n in cand_names}

    for ww in waterways:
        nm = _ref_name(ww.get("junction_b"))
        if nm in cand_names:
            refs[nm].append(("ww_b", ww))
        nm_a = _ref_name(ww.get("junction_a"))
        if nm_a in cand_names:
            refs[nm_a].append(BLOCK)
    for t in turbines:
        nm = _ref_name(t.get("junction_b"))
        if nm in cand_names:
            refs[nm].append(("turb_b", t))
        nm_a = _ref_name(t.get("junction_a"))
        if nm_a in cand_names:
            refs[nm_a].append(BLOCK)
    for f in system.get("flow_array", []):
        nm = _ref_name(f.get("junction"))
        if nm in cand_names:
            refs[nm].append(BLOCK)
    for r in system.get("reservoir_array", []):
        for k in ("junction", "spill_junction"):
            nm = _ref_name(r.get(k))
            if nm in cand_names:
                refs[nm].append(BLOCK)
    for fr in system.get("flow_right_array", []):
        for k in ("junction", "bypass_junction"):
            nm = _ref_name(fr.get(k))
            if nm in cand_names:
                refs[nm].append(BLOCK)

    collapsed: set[str] = set()
    for nm, rlist in refs.items():
        if not rlist or any(r == BLOCK for r in rlist):
            continue
        for _kind, el in rlist:
            el.pop("junction_b", None)
        collapsed.add(nm)

    if collapsed:
        system["junction_array"] = [j for j in juncs if j["name"] not in collapsed]
        logger.info(
            "build_planning: collapsed %d orphan drain sink junction(s) to "
            "outflow waterways/turbines (junction_b unset): %s",
            len(collapsed),
            ", ".join(sorted(collapsed)),
        )

    return len(collapsed)


def build_planning(
    case: PlexosCase,
    *,
    name: str,
    default_uc_penalty: float | None = None,
    lp_relax: bool = False,
    soft_efin_reservoirs: frozenset[str] = frozenset({"L_Maule"}),
    soft_penalty_override: float | None = None,
    fcf_scale_alpha: float | None = None,
    fcf_coeff_divisor: float = 1.0,
) -> dict[str, Any]:
    """Assemble the full gtopt planning JSON from a :class:`PlexosCase`.

    Empty per-class arrays are dropped from ``system`` (apart from
    the mandatory ``name``) so the JSON stays compact and the gtopt
    planning validator doesn't complain about empty schemas.
    """
    use_single_bus = len(case.nodes) <= 1
    fuel_array = build_fuel_array(case.fuels)
    emission_array = build_emission_array(case.fuels)
    # Synthesise the virtual unit-price Fuel when any generator emits
    # piecewise segments without a real Fuel-membership. The cost
    # writer pre-multiplies segment slopes by the per-generator
    # ``fuel_price_override``, so this fuel just needs ``price = 1``.
    if _needs_virtual_fuel(case.generators):
        fuel_array.append(
            {
                "uid": len(fuel_array) + 1,
                "name": VIRTUAL_FUEL_NAME,
                "price": 1.0,
            }
        )
    # Cheapest unserved-demand cost (VoLL) — caps the forced-floor
    # ``pmin_fcost`` below it so load-serving always outranks a forced
    # generation floor.  ``None`` when no demand carries a positive fcost.
    _demand_volls = [d.fcost for d in case.demands if d.fcost > 0.0]
    generator_array = build_generator_array(
        case.generators,
        case.fuels,
        generators_with_commitment=frozenset(
            c.generator_name for c in case.commitments
        ),
        block_layout=case.bundle.block_layout,
        demand_voll=min(_demand_volls) if _demand_volls else None,
        soft_penalty_override=soft_penalty_override,
    )
    demand_array = build_demand_array(
        case.demands,
        block_layout=case.bundle.block_layout,
    )
    # Soft-cap overload penalty for ex-EL0 lines: one quarter of the
    # calibrated slack cost ``(min demand.fcost + max gen.gcost)/2``
    # (~$285 → ~$71/MWh), so the LP pushes ~4 jumps into an EL0 line's
    # soft band before shedding load.  Computed from the bundle's own
    # cost data, not a magic constant.
    line_overload_penalty = (
        _compute_default_slack_cost(
            demand_array, generator_array, override=soft_penalty_override
        )
        / _LINE_OVERLOAD_DIVISOR
    )
    system: dict[str, Any] = {
        "name": name,
        "bus_array": build_bus_array(case.nodes),
        "generator_array": generator_array,
        "line_array": build_line_array(
            case.lines,
            block_layout=case.bundle.block_layout,
            overload_penalty=line_overload_penalty,
        ),
        "demand_array": demand_array,
        "battery_array": build_battery_array(
            case.batteries, block_layout=case.bundle.block_layout
        ),
        "fuel_array": fuel_array,
        "emission_array": emission_array,
        "junction_array": build_junction_array(case.junctions),
        "reservoir_array": build_reservoir_array(
            case.reservoirs, soft_efin_reservoirs=soft_efin_reservoirs
        ),
        # PLEXOS waterways model only physical spillage / scheduled
        # bypass now.  ``build_turbine_array`` emits each turbine as its
        # own built-in waterway (``junction_a``/``junction_b`` flow arc
        # + power conversion) — passing ``extra_waterways`` selects that
        # mode (vs. the legacy waterway-link path used by unit tests).
        "waterway_array": (
            waterway_array := build_waterway_array(
                case.waterways, block_layout=case.bundle.block_layout
            )
        ),
        "turbine_array": build_turbine_array(
            case.turbines,
            case.waterways,
            extra_waterways=waterway_array,
        ),
        "flow_array": build_flow_array(
            case.flows,
            block_layout=case.bundle.block_layout,
        ),
        "reserve_zone_array": build_reserve_zone_array(
            case.reserves,
            block_count=DEFAULT_BLOCK_COUNT * case.bundle.n_days,
            block_layout=case.bundle.block_layout,
        ),
        "reserve_provision_array": build_reserve_provision_array(
            case.reserve_provisions,
            block_layout=case.bundle.block_layout,
        ),
        "commitment_array": build_commitment_array(
            case.commitments,
            lp_relax=lp_relax,
            block_layout=case.bundle.block_layout,
        ),
        "flow_right_array": build_flow_right_array(
            case.flow_rights,
            block_count=DEFAULT_BLOCK_COUNT * case.bundle.n_days,
            block_layout=case.bundle.block_layout,
            extra_waterways=waterway_array,
            waterways=case.waterways,
        ),
        "decision_variable_array": build_decision_variable_array(
            case.decision_variables
        ),
        "plant_array": build_plant_array(case.plants),
        "user_constraint_array": build_user_constraint_array(
            case.user_constraints,
            default_penalty=default_uc_penalty,
            block_count=DEFAULT_BLOCK_COUNT * case.bundle.n_days,
            block_layout=case.bundle.block_layout,
        ),
    }
    # End-of-horizon future-cost valuation.  The ``boundary_cuts.csv``
    # path only binds under SDDP; for the monolithic LP we encode the
    # same FCF hyperplane explicitly as an ``alpha_fcf`` variable + a
    # ``FCF_future_cost`` user constraint, which DOES bind in monolithic.
    if case.boundary_cut is not None:
        # α-rebase state per reservoir (single cut, single scenario):
        # evaluate the boundary cut at the PRECISE end-volume target
        # ``efin`` — we expect the solution to hit it, so the cut value
        # ``c = FCF − Σ wv·efin`` is the cost-to-go AT that target and α'
        # centres on ~0 at the solution → minimal objective perturbance.
        # Falls back to the bound midpoint, then the initial volume, when
        # ``efin`` is absent.
        def _last_scalar(val: Any) -> float | None:
            while isinstance(val, list):
                if not val:
                    return None
                val = val[-1]
            return float(val) if isinstance(val, (int, float)) else None

        reservoir_state: dict[str, float] = {}
        for r in system.get("reservoir_array", []):
            efin_v = _last_scalar(r.get("efin"))
            if efin_v is not None:
                reservoir_state[r["name"]] = efin_v
                continue
            emin_v = _last_scalar(r.get("emin"))
            emax_v = _last_scalar(r.get("emax"))
            eini_v = _last_scalar(r.get("eini"))
            if emin_v is not None and emax_v is not None:
                reservoir_state[r["name"]] = 0.5 * (emin_v + emax_v)
            elif eini_v is not None:
                reservoir_state[r["name"]] = eini_v
            else:
                reservoir_state[r["name"]] = 0.0
        dv_arr = system.setdefault("decision_variable_array", [])
        uc_arr = system.setdefault("user_constraint_array", [])
        next_dv_uid = max((int(v["uid"]) for v in dv_arr), default=0) + 1
        next_uc_uid = max((int(u["uid"]) for u in uc_arr), default=0) + 1
        # Last block of the horizon (end-of-horizon, where `.efin` lives):
        # block uids are 1..N in build_simulation, so the last uid is the
        # block count.  ``alpha_fcf`` is an energy variable, so no
        # block-duration correction is needed.
        if case.bundle.block_layout:
            last_block_uid = len(case.bundle.block_layout)
        else:
            last_block_uid = int(case.bundle.step_count * case.bundle.n_days)
        fcf_terms = build_fcf_alpha_terms(
            case.boundary_cut,
            reservoir_state,
            dv_uid=next_dv_uid,
            uc_uid=next_uc_uid,
            last_block_uid=last_block_uid,
            scale_alpha=(
                fcf_scale_alpha if fcf_scale_alpha is not None else _FCF_SCALE_ALPHA
            ),
            coeff_divisor=fcf_coeff_divisor,
        )
        if fcf_terms is not None:
            alpha_dv, fcf_uc = fcf_terms
            dv_arr.append(alpha_dv)
            uc_arr.append(fcf_uc)
    # ── Default soft-EL=1 augmentation ────────────────────────────────
    # When the caller did NOT pass ``--lift-line-caps`` (empty env var),
    # add a parallel slack line on every EL=1 line so the LP has a
    # priced escape valve above the PLEXOS-stated rating.  This avoids
    # the all-or-nothing trade-off between "hard-cap EL=1 lines that
    # PLEXOS itself dispatches above rating" and "manually curate a
    # bundle-specific lift list" — the LP itself decides when to push
    # past the cap, paying the per-MWh slack penalty.
    #
    # Disabled when ``--lift-line-caps`` provides an explicit list:
    # those named lines are demoted to EL=0 inside ``extract_lines``
    # (the upstream behaviour), and the soft mode would be redundant.
    #
    # ``GTOPT_LIFT_LINE_CAPS`` is the same env var that
    # ``extract_lines`` reads; empty / unset → activate soft mode.
    import os as _os_soft

    _lift_raw = _os_soft.environ.get("GTOPT_LIFT_LINE_CAPS", "").strip()
    if not _lift_raw and system["line_array"]:
        penalty = _compute_default_slack_cost(
            system["demand_array"],
            system["generator_array"],
            override=soft_penalty_override,
        )
        n_softened = augment_el1_with_soft_caps(
            system["line_array"], overload_penalty=penalty
        )
        if n_softened > 0:
            logger.info(
                "augment_el1_with_soft_caps: softened %d EL=1 line(s) "
                "(tmax_normal_* ← rated, tmax_* ← 3× rated, "
                "overload_penalty=$%.2f/MWh = min(max(gcost)+1, min(VoLL)-1)); "
                "disable with --lift-line-caps=<list>",
                n_softened,
                penalty,
            )

    # Collapse orphan ``*_sink`` / ``*_ocean`` drain junctions whose only
    # purpose is to receive one waterway's outflow.  With the new Waterway /
    # Turbine ``junction_b`` optional ("outflow" mode), those consumers can
    # drain directly at ``junction_a`` and the synthetic sink junction can be
    # dropped.  Flow stays VISIBLE on the waterway / turbine — the same way
    # Vert_* spillages would, if they were emitted as waterways instead of
    # being collapsed onto Junction.drain.
    _collapse_orphan_drain_outflows(system)

    # Inline conversion-provenance: stamp every element with a coarse
    # ``type`` tag + a standardized ``description`` (source class, units,
    # files) so the planning JSON self-documents the PLEXOS→gtopt mapping
    # (F5, option A).  Reuses the per-class metadata that drives the
    # provenance sidecar; skips entries that already self-annotate
    # (Generator) and never adds ``type`` to UserConstraints (no field).
    _annotate_element_descriptions(system)

    # Drop empty arrays so the planning JSON stays compact and the
    # downstream JSON validator does not complain about empty schemas.
    system = {k: v for k, v in system.items() if v not in ((), [], {}) or k == "name"}

    return {
        "options": build_options(
            case.bundle,
            use_single_bus=use_single_bus,
            demand_fail_cost=case.bundle.demand_fail_cost,
        ),
        "simulation": build_simulation(case.bundle),
        "system": system,
    }


_BUNDLED_SOLVERS_DIR = Path(__file__).resolve().parent / "solvers"


def install_solver_param_files(output_dir: Path) -> list[Path]:
    """Copy every bundled ``<solver>.prm`` into ``<output_dir>/solvers/``.

    gtopt's ``prepare_matrix_options`` (source/gtopt_lp_runner.cpp) auto-loads
    ``<input_directory>/solvers/<solver_name>.prm`` when the user does not
    pass ``solver_options.param_file`` explicitly.  Shipping the curated
    parameter files alongside the JSON case means a fresh plexos2gtopt
    output is solver-tuned out of the box.

    Returns the list of installed file paths (empty when no bundled prm).
    """
    if not _BUNDLED_SOLVERS_DIR.is_dir():
        return []
    target_dir = output_dir / "solvers"
    target_dir.mkdir(parents=True, exist_ok=True)
    installed: list[Path] = []
    for src in sorted(_BUNDLED_SOLVERS_DIR.glob("*.prm")):
        dst = target_dir / src.name
        shutil.copyfile(src, dst)
        installed.append(dst)
        logger.info("installed solver param file: %s", dst)
    return installed


# Column scale for the FCF cost-to-go variable ``alpha_fcf``: the cut and
# objective coefficient on α is ``scale_alpha``, so α carries
# ``future_cost / scale_alpha``.  Tunable via ``--fcf-scale-alpha``
# (1 / 1e3 / 1e6 …).  Independent of the α-rebase shift (``obj_constant``
# and the cut RHS do not depend on ``scale_alpha``).
#
# DEFAULT = ``_DEFAULT_OBJ_SCALE`` (1000) so the alpha column shares the
# same scaling regime as ``scale_objective``.  Earlier LP-relax scans
# (DATOS20260422, K8 uniform, full UC) showed the reparametrization is
# benign for 1 ↔ 1e3 (identical optimum $956,155,053, ~85 s, kappa
# ~1.1e9); 1e6 BREAKS the solve (CPLEX aborts with garbage objective).
# Tying alpha to scale_objective keeps the LP coefficient on alpha
# matched to the rest of the objective — no asymmetric gradient
# magnitude that could degrade barrier conditioning.
_FCF_SCALE_ALPHA = _DEFAULT_OBJ_SCALE


def build_fcf_alpha_terms(
    boundary_cut: BoundaryCutSpec,
    reservoir_state: dict[str, float],
    *,
    dv_uid: int,
    uc_uid: int,
    last_block_uid: int,
    scale_alpha: float = _FCF_SCALE_ALPHA,
    coeff_divisor: float = 1.0,
) -> tuple[dict[str, Any], dict[str, Any]] | None:
    """Encode the FCF boundary cut as an alpha variable + user constraint.

    gtopt's ``boundary_cuts_file`` loader only feeds the SDDP path; in
    the monolithic LP the loaded cut is inert.  So we encode the same
    future-cost hyperplane explicitly, which DOES bind in monolithic:

      * ``alpha_fcf`` — the future cost-to-go in $, as a single
        last-block (``block`` scope), ENERGY (``cost_type`` = energy,
        cost = 1, NOT duration-weighted) DecisionVariable.
      * a UserConstraint
          ``alpha_fcf + Σ wv_r · reservoir(r).efin >= FCF``
        i.e. ``alpha_fcf >= FCF − Σ wv·efin``.  The objective pays
        ``FCF − Σ wv·efin``, rewarding terminal storage at each
        reservoir's water value — exactly PLEXOS's FCF first-order
        effect.  The constant ``FCF`` is α-rebased out (see below); the
        slopes ``wv_r`` drive the hydro/thermal trade-off.

    Returns ``(alpha_dv, fcf_uc)`` dicts, or ``None`` when no slope maps
    onto a bundle reservoir.
    """
    # ``coeff_divisor`` scales every water-value slope ``wv`` (the marginal
    # value of terminal storage).  divisor=2 halves the water values, making
    # hydro cheaper at the margin (more drawdown, less thermal back-fill) —
    # a sensitivity knob for the hydro/thermal trade-off.  Default 1.0 = off.
    cols = [
        (name, boundary_cut.slopes[name] / coeff_divisor)
        for name in boundary_cut.slopes
        if name in reservoir_state
    ]
    if not cols:
        return None
    # The FCF is a single END-OF-HORIZON future-cost cut: it values the
    # terminal (`.efin`) reservoir volumes, which only exist on the last
    # block.  Scope BOTH the cut (`for(block in {N})`, matched by
    # block.uid in user_constraint_lp.cpp:797) AND ``alpha_fcf`` itself
    # (``block`` scope) to that block, so each emits ONE row/column.
    # ``alpha_fcf`` is a RAW money variable (``cost_type`` = raw): the $
    # cost-to-go, present-valued by the discount factor only — NOT
    # probability- or duration-weighted.  cost = 1 reads it directly in
    # dollars (no ``cost = 1/duration`` magic correction).
    #
    # α-rebase (mean-shift) — mirrors SDDP boundary-cut loading and the
    # PLP convention: evaluate the cut at each reservoir's expected terminal
    # state (``reservoir_state`` = the ``efin`` target) to get ``c``, the
    # cost-to-go at that state, then substitute ``α = α' + c``:
    #     α' + Σ wv·efin >= FCF − c = Σ wv·state
    # Because the solution is expected to hit the ``efin`` target, α'
    # centres on ~0 AT THE SOLUTION → minimal objective perturbance, the LP
    # stays well-conditioned, and the relative MIP gap is meaningful (the
    # objective is no longer dominated by the raw ~1e9 intercept).  ``α'``
    # is FREE (the cut bounds it); safe because ``alpha_fcf`` is a SINGLE
    # last-block column.  The rebased-out ``c`` is added back verbatim via
    # ``add_obj_constant`` (``obj_constant``) so the reported objective is
    # the un-rebased value.  (Single cut, single scenario.)
    sum_slope_state = sum(wv * reservoir_state[name] for name, wv in cols)
    obj_constant = boundary_cut.fcf - sum_slope_state  # = c, cost-to-go @ efin
    shifted_rhs = sum_slope_state  # = FCF − c
    alpha_dv = {
        "uid": dv_uid,
        "name": "alpha_fcf",
        # FREE column: the FCF cut itself bounds α' (≥ −Σ wv·efin); after the
        # mean-shift α' may be negative, exactly as SDDP releases α to ±∞.
        "cost": scale_alpha,  # = 1; raw money → discount-only, not duration-weighted
        "cost_type": "raw",
        # Single last-block column (DecisionVariable.block scope) — the FCF
        # cost-to-go is one end-of-horizon variable, not one per block.
        "block": last_block_uid,
        # Mean-shift restitution: α = α' + c was rebased out of the
        # objective; the LP adds back obj_constant (= c) via
        # add_obj_constant so the reported objective is the un-rebased value.
        "obj_constant": obj_constant,
    }
    terms = [f'{scale_alpha:g} * decision_variable("alpha_fcf").value']
    terms += [f'{wv:.6f} * reservoir("{name}").efin' for name, wv in cols]
    expr = (
        " + ".join(terms) + f" >= {shifted_rhs:.6f}, for(block in {{{last_block_uid}}})"
    )
    fcf_uc = {
        "uid": uc_uid,
        "name": "FCF_future_cost",
        "expression": expr,
    }
    logger.info(
        "FCF future-cost: alpha_fcf (cost=%.0f) + %d reservoir water-value "
        "terms >= %.3e",
        scale_alpha,
        len(cols),
        boundary_cut.fcf,
    )
    return alpha_dv, fcf_uc


def write_boundary_cut_csv(
    boundary_cut: BoundaryCutSpec,
    reservoir_names: frozenset[str],
    output_dir: Path,
    *,
    scene: int = 0,
    filename: str = "boundary_cuts.csv",
) -> str | None:
    """Write the single FCF boundary cut as a gtopt ``boundary_cuts.csv``.

    Format (one cut, one scene)::

        scene,rhs,<res1>,<res2>,...
        0,<FCF>,-<wv1>,-<wv2>,...

    ``rhs`` is the FCF intercept; each reservoir coefficient is
    ``-water_value`` (more stored water ⇒ lower future cost) on that
    reservoir's terminal-volume state variable.  Only reservoirs that
    exist in the bundle are emitted (the loader matches by element
    name).  Returns the basename to set on ``simulation.boundary_cuts_file``,
    or ``None`` when no slope maps onto a bundle reservoir.
    """
    cols = [name for name in boundary_cut.slopes if name in reservoir_names]
    if not cols:
        logger.warning(
            "boundary cut: none of %d water-value reservoirs "
            "(%s) match a bundle reservoir — skipping cut",
            len(boundary_cut.slopes),
            ", ".join(sorted(boundary_cut.slopes)),
        )
        return None
    dropped = sorted(set(boundary_cut.slopes) - set(cols))
    if dropped:
        logger.info("boundary cut: skipping non-bundle reservoirs %s", dropped)
    path = output_dir / filename
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["scene", "rhs", *cols])
        writer.writerow(
            [
                scene,
                repr(boundary_cut.fcf),
                *(repr(-boundary_cut.slopes[c]) for c in cols),
            ]
        )
    logger.info(
        "wrote boundary cut: %s (scene=%d, rhs=%.3e, %d reservoir slopes)",
        path,
        scene,
        boundary_cut.fcf,
        len(cols),
    )
    return filename


# Per-class conversion provenance (F5, option B): documents — for every
# gtopt element class — its PLEXOS source class + files, the LP-variable /
# field units, and the transforms applied during conversion.  Static
# metadata; counts are filled from the built planning.  Keeps the LP JSON
# lean (no per-element strings) while giving a machine-readable record of
# WHAT each element is, its UNITS, and WHERE it came from.
_PROVENANCE_CLASS_DOC: dict[str, dict[str, Any]] = {
    "bus_array": {
        "gtopt": "Bus",
        "plexos": "Node",
        "files": ["DBSEN_PRGDIARIO.xml"],
        "units": {"voltage": "kV"},
        "transforms": ["one Bus per PLEXOS Node"],
    },
    "generator_array": {
        "gtopt": "Generator",
        "plexos": "Generator",
        "files": [
            "DBSEN_PRGDIARIO.xml",
            "Gen_Rating.csv",
            "Gen_HeatRate.csv",
            "Gen_FixedLoad.csv",
            "Gen_Commit.csv",
            "Fuel_Price.csv",
        ],
        "units": {
            "pmin": "MW",
            "pmax": "MW",
            "gcost": "$/MWh",
            "heat_rate": "fuel-unit/MWh",
            "pmin_fcost": "$/MWh",
            "lossfactor": "p.u.",
        },
        "transforms": [
            "FixedLoad → pmin/pmax (non-renewable: hard pmin=pmax; "
            "renewable/RoR: curtailable cap pmin=0)",
            "pmin_fcost = min(max(gcost)+1, min(VoLL)-1) (soft forced floor)",
            "Gen_Commit → Commitment.must_run / fixed_status",
        ],
    },
    "line_array": {
        "gtopt": "Line",
        "plexos": "Line",
        "files": ["DBSEN_PRGDIARIO.xml", "Lin_MaxRating.csv", "Lin_MinRating.csv"],
        "units": {
            "tmax_ab": "MW",
            "tmax_ba": "MW",
            "reactance": "p.u.",
            "resistance": "p.u.",
            "overload_penalty": "$/MWh",
        },
        "transforms": [
            "EL=1 lines optionally soft-capped (overload_penalty = "
            "min(max(gcost)+1, min(VoLL)-1))"
        ],
    },
    "demand_array": {
        "gtopt": "Demand",
        "plexos": "Node (Load)",
        "files": ["DBSEN_PRGDIARIO.xml", "Nod_Load.csv"],
        "units": {"capacity": "MW", "fcost": "$/MWh"},
        "transforms": ["per-Region VoLL → per-Demand fcost"],
    },
    "battery_array": {
        "gtopt": "Battery",
        "plexos": "Battery",
        "files": ["DBSEN_PRGDIARIO.xml", "BESS_IniValue.csv"],
        "units": {
            "pmax": "MW",
            "emax": "MWh",
            "eini": "MWh",
            "efin": "MWh",
        },
        "transforms": ["_AUX virtual reserve-buffer batteries dropped"],
    },
    "reservoir_array": {
        "gtopt": "Reservoir",
        "plexos": "Storage",
        "files": [
            "DBSEN_PRGDIARIO.xml",
            "Hydro_MaxVolume.csv",
            "Hydro_MinVolume.csv",
            "Hydro_InitialVolume.csv",
            "Hydro_StoWaterValues.csv",
        ],
        "units": {
            "vmin": "hm³",
            "vmax": "hm³",
            "vini": "hm³",
            "water_value": "$/hm³",
        },
        "transforms": [
            "1e30 Water Value → never-drain sentinel (water_value dropped)",
            "pondage/tailrace Storages demoted to Junction-only",
        ],
    },
    "waterway_array": {
        "gtopt": "Waterway",
        "plexos": "Waterway",
        "files": ["DBSEN_PRGDIARIO.xml", "Hydro_WaterFlows.csv"],
        "units": {"fmax": "m³/s", "fcost": "$/(m³/s)/h"},
        "transforms": [
            "forced-flow waterways → FlowRight at source junction",
            "Vert_* spillways collapsed onto Junction.drain",
        ],
    },
    "turbine_array": {
        "gtopt": "Turbine",
        "plexos": "Generator (hydro) + Waterway",
        "files": ["DBSEN_PRGDIARIO.xml", "Hydro_EfficiencyIncr.csv"],
        "units": {"production_factor": "MW/(m³/s)"},
        "transforms": [
            "turbine emitted as built-in waterway "
            "(junction_a/junction_b flow arc + power conversion); "
            "terminal plants drain (junction_b unset)"
        ],
    },
    "fuel_array": {
        "gtopt": "Fuel",
        "plexos": "Fuel",
        "files": ["DBSEN_PRGDIARIO.xml", "Fuel_Price.csv", "Fuel_MaxOfftakeWeek.csv"],
        "units": {
            "price": "$/fuel-unit",
            "max_offtake": "fuel-unit/period",
            "min_offtake": "fuel-unit/period",
        },
        "transforms": [
            "Fuel_MaxOfftakeWeek → Fuel.max_offtake (weekly budget)",
            (
                "Fuel.Min Offtake {Hour,Day,Week,Month,Year} → "
                "Fuel.min_offtake (horizon-wide budget, PLEXOS pids "
                "595-600); Min Offtake Penalty → min_offtake_cost "
                "with PLEXOS-faithful $1000/fuel-unit soft default "
                "when unset"
            ),
        ],
    },
    "junction_array": {
        "gtopt": "Junction",
        "plexos": "Storage (pass-through) / synthetic sink",
        "files": ["DBSEN_PRGDIARIO.xml"],
        "units": {"drain_capacity": "m³/s", "drain_cost": "$/(m³/s)/h"},
        "transforms": ["pass-through Storages + Vert_* sinks → Junction"],
    },
    "reserve_zone_array": {
        "gtopt": "ReserveZone",
        "plexos": "Reserve",
        "files": ["DBSEN_PRGDIARIO.xml", "Res_Requirement.csv"],
        "units": {"requirement": "MW"},
        "transforms": [],
    },
    "reserve_provision_array": {
        "gtopt": "ReserveProvision",
        "plexos": "Reserve → Generator eligibility",
        "files": ["DBSEN_PRGDIARIO.xml"],
        "units": {"up": "MW", "dn": "MW"},
        "transforms": ["zero-pmax gens filtered from eligibility"],
    },
    "commitment_array": {
        "gtopt": "Commitment",
        "plexos": "Generator UC params",
        "files": [
            "DBSEN_PRGDIARIO.xml",
            "Gen_Commit.csv",
            "Gen_StartCost.csv",
            "Gen_MinStableLevel.csv",
        ],
        "units": {
            "startup_cost": "$",
            "ramp_up": "MW/h",
            "ramp_down": "MW/h",
            "pmin": "MW",
            "initial_power": "MW",
        },
        "transforms": [
            "Max Ramp Up/Down MW/min → MW/h (×60)",
            "Gen_Commit ±1/0 → must_run / fixed_status",
        ],
    },
    "flow_array": {
        "gtopt": "Flow",
        "plexos": "Waterway forced flow",
        "files": ["DBSEN_PRGDIARIO.xml", "Hydro_WaterFlows.csv"],
        "units": {"fmin": "m³/s", "fmax": "m³/s"},
        "transforms": [],
    },
    "decision_variable_array": {
        "gtopt": "DecisionVariable",
        "plexos": "Decision Variable",
        "files": ["DBSEN_PRGDIARIO.xml"],
        "units": {"value": "per PLEXOS definition", "cost": "$/unit"},
        "transforms": [],
    },
    "user_constraint_array": {
        "gtopt": "UserConstraint",
        "plexos": "Constraint",
        "files": ["DBSEN_PRGDIARIO.xml", "Hydro_AntucoBounds.csv"],
        "units": {"rhs": "per-LHS-variable (see each constraint description)"},
        "transforms": [
            "RHS Custom → RHS × 1000 / horizon_hours (daily gas caps)",
            "_Uniq mutex groups → PlantCap config-exclusivity caps",
            "see each UserConstraint.description for per-row provenance",
        ],
    },
}


def _annotate_element_descriptions(system: dict[str, Any]) -> None:
    """Stamp a standardized conversion ``description`` (and a coarse
    ``type`` tag) onto every emitted element, in place.

    One source of truth: the per-class ``_PROVENANCE_CLASS_DOC`` (PLEXOS
    source class, field units, source files).  Entries that already
    carry a richer ``description`` (Generator, set in
    ``build_generator_array``) are left untouched.  ``type`` is set only
    where the gtopt schema has the field — i.e. NOT on UserConstraints
    (which carry ``description`` but no ``type``).
    """
    for array_key, doc in _PROVENANCE_CLASS_DOC.items():
        items = system.get(array_key, [])
        if not items:
            continue
        gtopt_cls = doc["gtopt"]
        unit_str = ", ".join(f"{k} [{v}]" for k, v in doc["units"].items())
        files = " + ".join(doc["files"][:3])
        type_tag = gtopt_cls.lower()
        sets_type = array_key != "user_constraint_array"
        for e in items:
            if sets_type:
                e.setdefault("type", type_tag)
            if not e.get("description"):
                name = e.get("name", "?")
                e["description"] = (
                    f"PLEXOS {doc['plexos']} '{name}' → gtopt {gtopt_cls}"
                    f"{('; ' + unit_str) if unit_str else ''}; (File: {files})"
                )


def build_provenance(
    planning: dict[str, Any],
    *,
    source_bundle: str = "",
) -> dict[str, Any]:
    """Build the conversion-provenance sidecar (F5, option B).

    Per gtopt element class: PLEXOS source class + files, field/LP-variable
    units, transforms applied, and the emitted count.  Plus global horizon
    and the cross-cutting numeric transforms (with formulas).
    """
    system = planning.get("system", {})
    model_opts = planning.get("options", {}).get("model_options", {})
    elements: dict[str, Any] = {}
    for array_key, doc in _PROVENANCE_CLASS_DOC.items():
        items = system.get(array_key, [])
        elements[doc["gtopt"]] = {
            "plexos_source": doc["plexos"],
            "count": len(items),
            "units": doc["units"],
            "source_files": doc["files"],
            "transforms": doc["transforms"],
        }
    return {
        "_comment": (
            "plexos2gtopt conversion provenance — what each gtopt element is, "
            "its units, source PLEXOS files, and transforms.  Companion to the "
            "planning JSON; not consumed by gtopt."
        ),
        "source_bundle": source_bundle,
        "demand_fail_cost": model_opts.get("demand_fail_cost"),
        "global_transforms": {
            "soft_penalty": "min(max(gcost)+1, min(VoLL)-1) — shared by "
            "Generator.pmin_fcost and the line-overload/EL=1 slack cost",
            "rhs_custom": "RHS_Custom × 1000 / horizon_hours — daily gas caps",
            "config_exclusivity": "PLEXOS *_Uniq mutex groups → PlantCap caps",
            "fixed_load": "non-renewable → hard pmin=pmax; renewable → "
            "curtailable cap (pmin=0)",
        },
        "elements": elements,
    }


def write_provenance(provenance: dict[str, Any], output_path: Path) -> Path:
    """Write the provenance sidecar JSON next to the planning file."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(provenance, fh, indent=2)
        fh.write("\n")
    logger.info("wrote conversion provenance: %s", output_path)
    return output_path


def _pampl_ident(name: str) -> str:
    """Sanitise a PLEXOS constraint name into a PAMPL ``IDENT``.

    PAMPL identifiers are ``[A-Za-z_][A-Za-z0-9_]*``; PLEXOS names may start
    with a digit (``2024084134_…``) or contain ``+ > - . space``.  Replace
    every invalid char with ``_`` and prefix ``uc_`` when the result would
    not start with a letter/underscore.  The original name is preserved in
    the constraint's description for traceability.
    """
    safe = re.sub(r"[^A-Za-z0-9_]", "_", name)
    if not safe or not (safe[0].isalpha() or safe[0] == "_"):
        safe = "uc_" + safe
    return safe


# Canonical names for the converter's penalty tiers — good-AMPL named
# constants instead of inline magic numbers.  Any other distinct PLEXOS
# penalty gets a value-derived name (``penalty_467_19`` …).
_PENALTY_TIER_NAMES: dict[float, str] = {10.0: "soft_floor_penalty"}
# Source of the PLEXOS Constraint objects emitted to ``.pampl`` (the model
# DB).  Recorded in each file/constraint comment for traceability.
_UC_ORIGIN_FILE = "DBSEN_PRGDIARIO.xml"


def _penalty_param_name(value: float) -> str:
    """PAMPL ``param`` name for a given per-unit penalty tier."""
    if value in _PENALTY_TIER_NAMES:
        return _PENALTY_TIER_NAMES[value]
    return "penalty_" + re.sub(r"[^0-9]", "_", f"{value:g}")


def _pampl_rhs_vector(rhs: Any) -> list[float] | None:
    """Return the per-block RHS vector if ``rhs`` is a TB-matrix profile.

    ``UserConstraint.rhs`` accepts several shapes; only the single-row
    TB-matrix form ``[[v0, v1, ...]]`` (what ``build_user_constraint_array``
    emits for a per-block profile) has a PAMPL ``rhs [v0, v1, ...]`` encoding.
    Returns the inner block vector for that shape, or ``None`` for scalar /
    per-stage / string / multi-stage forms that must stay inline in the JSON.
    """
    if (
        isinstance(rhs, list)
        and len(rhs) == 1
        and isinstance(rhs[0], list)
        and rhs[0]
        and all(isinstance(v, (int, float)) for v in rhs[0])
    ):
        return [float(v) for v in rhs[0]]
    return None


# Default slack penalty forced onto every UC under ``--pampl-uc-mode soft``
# when no ``--default-uc-penalty`` is supplied (matches the flag's "Typical"
# value).  High enough to keep load-serving optimal, low enough to stay below
# the cheapest unserved-demand cost.
_SOFT_UC_DEFAULT_PENALTY = 10000.0

# UC family taxonomy lives in the light, dependency-free ``uc_families``
# module so ``gtopt_check_json`` can import the classifier without pulling in
# this heavy writer.  Re-exported here for backward compatibility.


def filter_user_constraints(
    uc_array: list[dict[str, Any]],
    *,
    mode: str = "hard",
    force_penalty: float = _SOFT_UC_DEFAULT_PENALTY,
    only: frozenset[str] | None = None,
    off: frozenset[str] = frozenset(),
) -> list[dict[str, Any]]:
    """Apply ``--pampl-uc-mode`` / ``--pampl-uc-only`` / ``--pampl-uc-off`` to a
    UC list and return the kept rows (order preserved).

    Shared by both emit paths so the inline-JSON and ``.pampl`` encodings see
    an identical constraint set — the basis for diffing the two LPs.
    ``mode="soft"`` stamps ``force_penalty`` onto rows lacking one;
    ``mode="off"`` returns ``[]``.
    """
    if mode == "off":
        return []
    kept: list[dict[str, Any]] = []
    for uc in uc_array:
        fam = uc_family(str(uc.get("name", "")))
        if only is not None and fam not in only:
            continue
        if fam in off:
            continue
        if mode == "soft" and float(uc.get("penalty") or 0.0) <= 0.0:
            uc["penalty"] = force_penalty
        kept.append(uc)
    return kept


def write_user_constraint_pampl(
    uc_array: list[dict[str, Any]],
    output_dir: Path,
    *,
    mode: str = "hard",
    force_penalty: float = _SOFT_UC_DEFAULT_PENALTY,
    only: frozenset[str] | None = None,
    off: frozenset[str] = frozenset(),
) -> tuple[list[str], list[dict[str, Any]]]:
    """Split user constraints into modular per-family ``.pampl`` files.

    Scalar-RHS constraints are emitted as
    ``[inactive ]constraint NAME ["desc"][ penalty N]: <expr>;`` grouped by
    family (``uc_config_exclusivity.pampl`` …).  Constraints carrying a
    per-block ``rhs`` profile in TB-matrix form (``[[v0, v1, ...]]`` — the
    day-scoped gas caps, curtailment shifts, hydro daily-ramp rows like
    ``RALCOramp_max_e1``) are emitted with a ``rhs [v0, v1, ...]`` header
    clause so they round-trip through ``.pampl`` too.  Any other RHS shape
    (scalar / per-stage / string / multi-stage matrix) has no ``.pampl``
    encoding yet, so those stay inline in the JSON.  Returns
    ``(pampl_filenames, json_remaining)``.

    ``mode`` (from ``--pampl-uc-mode``) controls the hard/soft/off policy:

    - ``"hard"`` (default): each row keeps its own ``penalty`` (rows without
      one stay hard equalities/inequalities).
    - ``"soft"``: every row is forced soft — any row lacking a positive
      ``penalty`` gets ``force_penalty`` — so the MIP stays feasible and
      reports violations instead of going infeasible.
    - ``"off"``: emit nothing and drop every row (both the per-family files
      and the inline per-block-RHS rows); returns ``([], [])``.

    Per-group selection (from ``--pampl-uc-only`` / ``--pampl-uc-off``):

    - ``only``: when given, keep ONLY rows whose family is in this set (every
      other family is dropped) — used to isolate one group at a time.
    - ``off``: drop rows whose family is in this set (leave-one-out).
    """
    if mode == "off":
        return [], []
    kept = filter_user_constraints(
        uc_array, mode=mode, force_penalty=force_penalty, only=only, off=off
    )
    families: dict[str, list[dict[str, Any]]] = {}
    json_remaining: list[dict[str, Any]] = []
    for uc in kept:
        # Daily-ENERGY budgets (PLEXOS ``RHS Day`` / ramp-day) carry a
        # ``daily_sum`` / ``constraint_type`` flag that the ``.pampl`` grammar
        # (``pampl_parser.cpp``: only ``penalty`` + ``rhs`` clauses) cannot
        # express, so they stay inline in the JSON ``user_constraint_array``.
        if uc.get("daily_sum"):
            json_remaining.append(uc)
            continue
        # A per-block RHS profile in TB-matrix form (``[[v0, v1, ...]]`` — the
        # shape ``build_user_constraint_array`` emits) maps onto the PAMPL
        # ``rhs [v0, v1, ...]`` header clause, so it can now round-trip
        # through ``.pampl``.  Scalar / per-stage / string RHS forms have no
        # scalar ``.pampl`` encoding yet, so those stay inline in the JSON.
        if "rhs" in uc and _pampl_rhs_vector(uc["rhs"]) is None:
            json_remaining.append(uc)
            continue
        families.setdefault(uc_family(str(uc.get("name", ""))), []).append(uc)

    # PAMPL identifiers must be unique across every loaded constraint.
    # Sanitisation (``+ > - space`` → ``_``) can collapse distinct PLEXOS
    # names onto one ident, so dedupe globally with a numeric suffix; the
    # original name is preserved in the description.
    seen_idents: set[str] = set()

    def _unique_ident(raw: str) -> str:
        base = _pampl_ident(raw)
        cand = base
        suffix = 2
        while cand in seen_idents:
            cand = f"{base}_{suffix}"
            suffix += 1
        seen_idents.add(cand)
        return cand

    filenames: list[str] = []
    for fam, ucs in sorted(families.items()):
        fname = f"uc_{fam}.pampl"
        # Distinct soft-penalty tiers used in this file → declared once as
        # named ``param`` constants and referenced below (no magic numbers).
        pens = sorted(
            {
                round(float(uc.get("penalty") or 0.0), 6)
                for uc in ucs
                if (uc.get("penalty") or 0.0) > 0.0
            }
        )
        # Count soft / hard rows for the file header banner — gives a reader
        # an at-a-glance summary of how many slack columns this file
        # contributes to the LP.
        n_soft = sum(1 for uc in ucs if (uc.get("penalty") or 0.0) > 0.0)
        n_hard = len(ucs) - n_soft

        # Pre-resolve each UC's PAMPL ident so the matching
        # ``var slack_<ident>;`` declaration below uses the EXACT name
        # the constraint header will use (after sanitisation + dedup).
        # The PAMPL parser binds slacks by naming convention
        # (``slack_<NAME>`` matches constraint ``NAME``), so the var
        # declaration MUST match the constraint name byte-for-byte.
        uc_idents: list[tuple[dict[str, Any], str, float]] = []
        for uc in ucs:
            uc_name = str(uc.get("name", ""))
            ident = _unique_ident(uc_name)
            uc_idents.append((uc, ident, float(uc.get("penalty") or 0.0)))

        lines = [
            "# " + "=" * 72,
            f"# {fam} user constraints ({len(ucs)}) — emitted by plexos2gtopt",
            f"#   hard: {n_hard:>5}   soft: {n_soft:>5}",
            f"# Origin: PLEXOS Constraint objects ({_UC_ORIGIN_FILE})",
            "# Soft rows declare a named slack column via the AMPL-style",
            "# ``var slack_<ident>;`` syntax — the PAMPL parser binds each",
            "# declaration to the matching ``constraint <ident>`` by naming",
            "# convention (gtopt-side: ``UserConstraint::slack_name``), so",
            "# CPLEX logs and LP dumps reference the per-UC slack label",
            "# instead of the generic ``slack``.  The output parquet schema",
            "# is unchanged — per-row violations live in",
            "# ``UserConstraint/slack_sol.parquet`` keyed by uid.",
            "# " + "=" * 72,
            "",
        ]
        if pens:
            lines.append("# Penalty tiers — per-unit slack cost [$/unit]:")
            lines.extend(f"param {_penalty_param_name(p)} = {p:g};" for p in pens)
            lines.append("")
        # Visible-slack declarations: one ``var slack_<ident>;`` per soft
        # UC, grouped in a single block at the top of the file so a
        # reader sees every named slack the file contributes to the LP
        # before walking the constraints.  Hard rows are skipped (no
        # slack column).  When the file has no soft rows the block
        # collapses to an empty section.
        soft_idents = [ident for _uc, ident, pen in uc_idents if pen > 0.0]
        if soft_idents:
            lines.append("# Visible slack columns — one per soft constraint.")
            for ident in soft_idents:
                lines.append(f"var slack_{ident};")
            lines.append("")
        for uc, ident, pen in uc_idents:
            # Per-constraint comment carries the PLEXOS semantics + origin
            # file (from the description) for traceability.
            lines.append(f"# {uc.get('description') or uc.get('name', '')}")
            # Visible-slack annotation: soft rows tell the reader where the
            # LP exposes the per-block violation.  The matching
            # ``var slack_<ident>;`` declaration above seeds
            # ``UserConstraint::slack_name`` on the gtopt side so CPLEX
            # logs use the per-UC label.  When a typed ``directive`` is
            # attached, surface its ``kind`` so a reader can attribute
            # the soft tier to a family without grepping ``_uc_policy.py``.
            uc_name = str(uc.get("name", ""))
            if pen > 0.0:
                lines.append(
                    f"#   soft: slack column 'slack_{ident}' (per-block; "
                    f'see UserConstraint/slack_sol.parquet["{uc_name}"])'
                )
            directive = uc.get("directive")
            if isinstance(directive, dict) and directive.get("kind"):
                kind = directive["kind"]
                extra: list[str] = []
                if directive.get("scope"):
                    extra.append(f"scope={directive['scope']}")
                if directive.get("window_hours") is not None:
                    extra.append(f"window_hours={directive['window_hours']}")
                tail = (" " + ", ".join(extra)) if extra else ""
                lines.append(f"#   directive: kind={kind}{tail}")
            prefix = "inactive " if uc.get("active") is False else ""
            pen_txt = (
                f" penalty {_penalty_param_name(round(pen, 6))}" if pen > 0.0 else ""
            )
            rhs_txt = ""
            if "rhs" in uc:
                rhs_vec = _pampl_rhs_vector(uc["rhs"])
                if rhs_vec is not None:
                    rhs_txt = " rhs [" + ", ".join(f"{v:g}" for v in rhs_vec) + "]"
            lines.append(f"{prefix}constraint {ident}{pen_txt}{rhs_txt}:")
            lines.append(f"  {uc['expression']};")
            lines.append("")
        (output_dir / fname).write_text("\n".join(lines), encoding="utf-8")
        filenames.append(fname)
        logger.info("wrote %d %s constraint(s) → %s", len(ucs), fam, fname)
    return filenames, json_remaining


def write_planning(planning: dict[str, Any], output_path: Path) -> Path:
    """Write the planning to ``output_path`` (parents created on demand).

    Also installs any bundled ``<solver>.prm`` files into
    ``<output_path.parent>/solvers/`` so that ``gtopt`` picks up tuned
    backend parameters automatically.
    """
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(planning, fh, indent=2)
        fh.write("\n")
    logger.info("wrote gtopt planning: %s", output_path)
    install_solver_param_files(output_path.parent)
    return output_path


__all__ = [
    "DEFAULT_BLOCK_COUNT",
    "DEFAULT_BLOCK_DURATION_H",
    "DEFAULT_FP_MED",
    "UC_FAMILY_NAMES",
    "build_battery_array",
    "build_bus_array",
    "build_commitment_array",
    "build_demand_array",
    "build_emission_array",
    "build_flow_array",
    "build_flow_right_array",
    "build_fuel_array",
    "build_generator_array",
    "build_junction_array",
    "build_line_array",
    "build_options",
    "build_plant_array",
    "build_planning",
    "build_provenance",
    "build_reservoir_array",
    "build_reserve_provision_array",
    "build_reserve_zone_array",
    "build_simulation",
    "build_turbine_array",
    "build_user_constraint_array",
    "build_waterway_array",
    "install_solver_param_files",
    "uc_family",
    "write_planning",
    "write_provenance",
]
