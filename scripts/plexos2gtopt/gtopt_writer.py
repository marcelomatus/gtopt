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

import json
import logging
from pathlib import Path
from typing import Any

from .entities import (
    BatterySpec,
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
    PlexosCase,
    ReservoirSpec,
    ReserveProvisionSpec,
    ReserveSpec,
    TurbineSpec,
    UserConstraintSpec,
    WaterwaySpec,
)


logger = logging.getLogger(__name__)


# CEN PCP horizon: one operating day = 24 hourly blocks.
DEFAULT_BLOCK_COUNT = 24
DEFAULT_BLOCK_DURATION_H = 1.0

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
        "input_format": "parquet",
        "output_format": "parquet",
        "output_compression": "snappy",
        "model_options": {
            "use_single_bus": use_single_bus,
            "use_kirchhoff": not use_single_bus,
            "scale_objective": 1000,
            "demand_fail_cost": demand_fail_cost,
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


def build_generator_array(
    generators: tuple[GeneratorSpec, ...],
    fuels: tuple[FuelSpec, ...] = (),
    generators_with_commitment: frozenset[str] = frozenset(),
    *,
    block_layout: tuple[tuple[int, ...], ...] = (),
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
            "pmin": gen.pmin,
        }
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
        out.append(entry)
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


def build_line_array(
    lines: tuple[LineSpec, ...],
    *,
    block_layout: tuple[tuple[int, ...], ...] = (),
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
                import os as _os

                try:
                    nseg = int(_os.environ.get("GTOPT_NSEG_LOSSES", "3"))
                except ValueError:
                    nseg = 3
                entry["loss_segments"] = nseg if nseg >= 1 else 3
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
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": bat.name,
            "bus": bat.bus_name,
            "emin": bat.emin,
            "emax": bat.emax,
            "eini": eini,
            "efin": bat.efin,
        }
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
        out.append(entry)
    return out


def build_reservoir_array(
    reservoirs: tuple[ReservoirSpec, ...],
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
            if not res.never_drain and res.water_value > 0.0:
                # Soft slack — per-reservoir Water Value as the
                # shortfall penalty.  Units match efin (PLEXOS GWh).
                entry["efin_cost"] = res.water_value
            # ``never_drain`` branch omits efin_cost → gtopt builds the
            # row as ``vol_end >= efin`` HARD; the LP must hit the
            # target or fail.
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
    generators: tuple[GeneratorSpec, ...] = (),
    extra_junctions: list[dict[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    """One Turbine per :class:`TurbineSpec`.

    Each entry links a Generator (electrical output) to its upstream
    Reservoir (water balance via ``main_reservoir``).  When a Waterway
    is registered with ``storage_from`` matching the turbine's main
    reservoir, we set ``waterway`` to that Waterway — gtopt needs
    either ``waterway`` or ``flow`` set, otherwise it emits
    ``Turbine uid=…: no waterway or flow reference`` and the turbine's
    LP rows are empty.

    PLEXOS ``Vert_*`` Waterways are *spillways* (vertimiento — bypass
    water that wastes potential energy) priced with a per-flow penalty
    (CEN PCP uses ``fcost=3.6 $/(m³/s)/h``).  Turbines must NOT route
    through the spillway; they need their own zero-cost penstock from
    the reservoir to the downstream junction.  This routine therefore
    **synthesizes a fresh penstock waterway per turbine** with
    ``fcost=0`` (no spill penalty on turbine flow) and connects each
    turbine to its own private penstock.  The original PLEXOS
    Waterways are left untouched in the JSON and continue to model
    physical spillage / scheduled bypass flows independently.

    Side-effect: ``extra_waterways`` receives one new entry per
    eligible turbine.  Without this, multi-unit plants like ANTUCO
    (U1/U2), MACHICURA (U1/U2), PEHUENCHE (U1/U2), etc. would all
    share the spillway and the per-turbine ``gen = waterway_flow``
    equations would force ``gen_u1 = gen_u2 = …``, infeasible when
    the units' commitment pmin / pmax differ.

    Turbines without a downstream waterway (terminal hydro plants like
    CANUTILLAR / RAPEL / ANGOSTURA + thermal pseudo-turbines on gas
    Storage) are dropped with a summary log line; gtopt's validator
    rejects a Turbine that references neither a Waterway nor a Flow.
    """
    ww_by_from: dict[str, str] = {}
    ww_by_name: dict[str, WaterwaySpec] = {}
    for w in waterways:
        ww_by_name[w.name] = w
        if w.storage_from and w.storage_from not in ww_by_from:
            ww_by_from[w.storage_from] = w.name

    # Per-generator peak pmax (max across stages / blocks).  Used to
    # cap the synthetic penstock ``fmax`` at ``pmax_peak / pf`` so
    # the waterway can't carry more water than the unit could
    # physically discharge at its nameplate.  Closes the v22 leak
    # where ``EL_TORO_U1`` (pmax_profile range [0, 113.4]) had its
    # gen column skipped at the zero-pmax blocks, leaving the
    # penstock unbounded and letting ELTORO drain ~12,000 hm³
    # through it.
    def _peak_pmax(g: GeneratorSpec) -> float:
        if g.pmax_profile:
            return max(g.pmax_profile, default=0.0)
        return g.pmax or 0.0

    gen_peak_pmax: dict[str, float] = {g.name: _peak_pmax(g) for g in generators}
    out: list[dict[str, Any]] = []
    skipped: list[str] = []
    synthesised_count = 0
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
        if downstream is None:
            # Terminal hydro turbine (last station in its basin —
            # LAJA_I, ANGOSTURA_U1/U2/U3, CANUTILLAR_U1/U2, RAPEL_U1..U5
            # on CEN PCP).  Previously dropped, which left the
            # generator UNCOUPLED from water — gcost=0 + no turbine
            # link meant the LP could dispatch up to nameplate at zero
            # cost without consuming any cascade water.  Synthesize a
            # per-turbine ocean drain junction + penstock so the LP
            # gen↔water equality (``gen = pf × penstock_flow``)
            # binds.  Only fires when ``extra_junctions`` is provided
            # (build_planning call site); legacy callers without it
            # still get the old skip behaviour.
            if extra_junctions is not None and extra_waterways is not None:
                drain_junction = f"{t.reservoir_name}_terminal_ocean"
                if not any(j.get("name") == drain_junction for j in extra_junctions):
                    extra_junctions.append(
                        {
                            "uid": 0,  # patched in build_planning
                            "name": drain_junction,
                            "drain": True,
                            "drain_cost": 0.0,
                        }
                    )
                downstream = drain_junction
            else:
                skipped.append(t.generator_name)
                continue
        # When ``extra_waterways`` is provided (build_planning call
        # site), synthesise a per-turbine zero-cost penstock so each
        # gen has its own ``waterway_flow_*`` variable and never
        # shares the PLEXOS spillway.  Without an extras list (legacy
        # callers / unit tests), keep the legacy behaviour of linking
        # the turbine to the original PLEXOS waterway.
        if extra_waterways is not None:
            penstock_name = f"penstock_{t.generator_name}"
            penstock_entry: dict[str, Any] = {
                "uid": 0,  # patched in build_planning
                "name": penstock_name,
                "junction_a": t.reservoir_name,
                "junction_b": downstream,
            }
            # Cap the synthetic penstock's flow at the unit's peak
            # discharge capacity ``pmax / pf``.  Without this cap
            # the waterway is unbounded; at blocks where the
            # generator's pmax is 0 the turbine_conversion row is
            # skipped (no gen col → no `gen = pf × flow` equality)
            # and the LP can push arbitrary water through the
            # penstock for free.  pmax_peak is taken across the
            # whole horizon — per-block fmax profiles would be
            # tighter but require WaterwaySpec schema work.
            pmax_peak = gen_peak_pmax.get(t.generator_name, 0.0)
            if pmax_peak > 0.0 and t.production_factor > 0.0:
                penstock_entry["fmax"] = pmax_peak / t.production_factor
            extra_waterways.append(penstock_entry)
            synthesised_count += 1
            waterway_ref = penstock_name
        else:
            waterway_ref = ww_by_from.get(t.reservoir_name) or ""
            if not waterway_ref:
                skipped.append(t.generator_name)
                continue
        entry: dict[str, Any] = {
            "uid": len(out) + 1,
            "name": f"turbine_{t.generator_name}",
            "generator": t.generator_name,
            "main_reservoir": t.reservoir_name,
            "waterway": waterway_ref,
        }
        if t.production_factor > 0.0:
            entry["production_factor"] = t.production_factor
        out.append(entry)
    if synthesised_count:
        logger.info(
            "build_turbine_array: synthesised %d zero-cost per-turbine "
            "penstock waterways (one per turbine, leaving the PLEXOS "
            "Vert_*/Ext_*/Filt_*/Caudal_* spillways untouched as "
            "physical spillage paths).",
            synthesised_count,
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
        if block_layout:
            return _aggregate_to_blocks(hourly, block_layout, reducer="mean")
        if len(hourly) == target_len:
            return hourly
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
) -> list[dict[str, Any]]:
    """One ``Commitment`` per :class:`CommitmentSpec`.

    ``relax: true`` is set on every entry so the LP-only path solves
    without an MIP solver. Drop the flag (or override via JSON) when
    you have CPLEX/CBC/HiGHS MIP support and want full binary
    commitment decisions.
    """
    out: list[dict[str, Any]] = []
    for i, c in enumerate(commitments):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": f"uc_{c.generator_name}",
            "generator": c.generator_name,
            "relax": True,
        }
        if c.startup_cost > 0.0:
            entry["startup_cost"] = c.startup_cost
        if c.shutdown_cost > 0.0:
            entry["shutdown_cost"] = c.shutdown_cost
        if c.min_up_time > 0.0:
            entry["min_up_time"] = c.min_up_time
        if c.min_down_time > 0.0:
            entry["min_down_time"] = c.min_down_time
        if c.ramp_up > 0.0:
            entry["ramp_up"] = c.ramp_up
        if c.ramp_down > 0.0:
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
        if c.pmin > 0.0:
            entry["pmin"] = c.pmin
        out.append(entry)
    return out


def build_reserve_provision_array(
    provisions: tuple[ReserveProvisionSpec, ...],
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
    out: list[dict[str, Any]] = []
    for i, p in enumerate(provisions):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": f"provision_{p.generator_name}",
            "generator": p.generator_name,
            "reserve_zones": list(p.reserve_zones),
            "ur_provision_factor": 1.0,
            "dr_provision_factor": 1.0,
        }
        if p.urmax > 0.0:
            entry["urmax"] = p.urmax
        if p.drmax > 0.0:
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


def build_planning(
    case: PlexosCase,
    *,
    name: str,
    default_uc_penalty: float | None = None,
) -> dict[str, Any]:
    """Assemble the full gtopt planning JSON from a :class:`PlexosCase`.

    Empty per-class arrays are dropped from ``system`` (apart from
    the mandatory ``name``) so the JSON stays compact and the gtopt
    planning validator doesn't complain about empty schemas.
    """
    use_single_bus = len(case.nodes) <= 1
    fuel_array = build_fuel_array(case.fuels)
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
    system: dict[str, Any] = {
        "name": name,
        "bus_array": build_bus_array(case.nodes),
        "generator_array": build_generator_array(
            case.generators,
            case.fuels,
            generators_with_commitment=frozenset(
                c.generator_name for c in case.commitments
            ),
            block_layout=case.bundle.block_layout,
        ),
        "line_array": build_line_array(
            case.lines,
            block_layout=case.bundle.block_layout,
        ),
        "demand_array": build_demand_array(
            case.demands,
            block_layout=case.bundle.block_layout,
        ),
        "battery_array": build_battery_array(case.batteries),
        "fuel_array": fuel_array,
        "junction_array": (junction_array := build_junction_array(case.junctions)),
        "reservoir_array": build_reservoir_array(case.reservoirs),
        # Order matters: build the waterway list first, then let
        # build_turbine_array APPEND any per-unit clone waterways it
        # synthesises for multi-unit plants (see docstring on
        # build_turbine_array for the rationale).  ``junction_array``
        # is also passed so terminal hydro turbines can synthesise a
        # ``<reservoir>_terminal_ocean`` drain Junction on demand.
        "waterway_array": (
            waterway_array := build_waterway_array(
                case.waterways, block_layout=case.bundle.block_layout
            )
        ),
        "turbine_array": build_turbine_array(
            case.turbines,
            case.waterways,
            extra_waterways=waterway_array,
            generators=case.generators,
            extra_junctions=junction_array,
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
            case.reserve_provisions
        ),
        "commitment_array": build_commitment_array(case.commitments),
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
        "user_constraint_array": build_user_constraint_array(
            case.user_constraints,
            default_penalty=default_uc_penalty,
            block_count=DEFAULT_BLOCK_COUNT * case.bundle.n_days,
            block_layout=case.bundle.block_layout,
        ),
    }
    # Assign sequential UIDs to any per-unit waterway clones that
    # `build_turbine_array` appended with placeholder uid=0.
    next_ww_uid = max((w.get("uid", 0) for w in waterway_array), default=0) + 1
    for w in waterway_array:
        if w.get("uid", 0) == 0:
            w["uid"] = next_ww_uid
            next_ww_uid += 1
    # Same renumbering for any ``<X>_terminal_ocean`` junctions that
    # ``build_turbine_array`` synthesised for terminal hydro turbines.
    next_j_uid = max((j.get("uid", 0) for j in junction_array), default=0) + 1
    for j in junction_array:
        if j.get("uid", 0) == 0:
            j["uid"] = next_j_uid
            next_j_uid += 1
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


def write_planning(planning: dict[str, Any], output_path: Path) -> Path:
    """Write the planning to ``output_path`` (parents created on demand)."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(planning, fh, indent=2)
        fh.write("\n")
    logger.info("wrote gtopt planning: %s", output_path)
    return output_path


__all__ = [
    "DEFAULT_BLOCK_COUNT",
    "DEFAULT_BLOCK_DURATION_H",
    "DEFAULT_FP_MED",
    "build_battery_array",
    "build_bus_array",
    "build_commitment_array",
    "build_demand_array",
    "build_flow_array",
    "build_flow_right_array",
    "build_fuel_array",
    "build_generator_array",
    "build_junction_array",
    "build_line_array",
    "build_options",
    "build_planning",
    "build_reservoir_array",
    "build_reserve_provision_array",
    "build_reserve_zone_array",
    "build_simulation",
    "build_turbine_array",
    "build_user_constraint_array",
    "build_waterway_array",
    "write_planning",
]
