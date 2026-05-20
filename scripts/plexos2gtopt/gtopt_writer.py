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
    block_count: int = DEFAULT_BLOCK_COUNT,
    block_duration_h: float = DEFAULT_BLOCK_DURATION_H,
) -> dict[str, Any]:
    """Emit the (1 scenario × 1 stage × N blocks) chronological skeleton.

    Matches both ``cases/c0`` and ``tools/ucjl2gtopt.py``: gtopt's
    simulation block needs only ``block_array``, ``stage_array``,
    ``scenario_array``. Scenes and phases are inferred by the solver
    when absent (single scene, single phase).
    """
    _ = bundle  # reserved for future use (bundle date stamps, calendar tags)
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
        else:
            # Scalar path: pre-baked gcost coefficient.
            gcost = gen.heat_rate * primary_price + gen.vom_charge + gen.fuel_transport
            entry["gcost"] = gcost
        # When the per-hour rating profile actually varies (renewable
        # availability, scheduled maintenance) emit the profile as a
        # [[stage_blocks]] matrix so the LP honours it block-by-block.
        # Constant profiles collapse to the scalar max.
        if gen.pmax_profile and (max(gen.pmax_profile) != min(gen.pmax_profile)):
            entry["pmax"] = [list(gen.pmax_profile)]
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


def build_line_array(lines: tuple[LineSpec, ...]) -> list[dict[str, Any]]:
    """One line entry per :class:`LineSpec`.

    gtopt's `Line` carries forward/reverse capacity as ``tmax_ab`` /
    ``tmax_ba`` (both non-negative MW). PLEXOS' ``Lin_MinRating.csv``
    ships the reverse-flow limit as a negative number; we flip the
    sign and multiply by the parallel-line count.
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
        # PLEXOS Enforce Limits = 0 ("Never") means thermal limits are
        # not enforced — the LP should treat the line as having no cap.
        # Drop tmax_ab/tmax_ba so gtopt defaults to +inf.  For EL=1
        # (Voltage-conditional) and EL=2 (Always), emit the hard cap
        # as before — gtopt has no voltage-threshold construct so
        # EL=1 is conservatively treated as enforced.
        if line.enforce_limits != 0:
            entry["tmax_ab"] = line.tmax_ab * units
            entry["tmax_ba"] = (
                abs(line.tmin_ab) * units if line.tmin_ab else line.tmax_ab * units
            )
        if line.reactance > 0.0:
            entry["reactance"] = line.reactance
        # PLEXOS Wheeling Charge ($/MWh) → gtopt Line.tcost.
        if line.wheeling_charge > 0.0:
            entry["tcost"] = line.wheeling_charge
        out.append(entry)
    return out


def build_demand_array(demands: tuple[DemandSpec, ...]) -> list[dict[str, Any]]:
    """One demand entry per :class:`DemandSpec`, with inline ``lmax`` matrix.

    The PCP daily horizon fits comfortably inline (127 demands × 24
    blocks ≈ 24 kB of JSON); Parquet sidecar emission would only pay
    off for multi-stage horizons.
    """
    out: list[dict[str, Any]] = []
    for i, dem in enumerate(demands):
        entry: dict[str, Any] = {
            "uid": i + 1,
            "name": dem.name,
            "bus": dem.bus_name,
            # Inline lmax: gtopt expects [[stage_0_blocks], ...]; for the
            # PCP single-stage horizon this is a 1×N matrix.
            "lmax": [list(dem.lmax_profile)],
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
        }
        if res.emin > 0.0:
            entry["emin"] = res.emin
        if res.emax > 0.0:
            entry["emax"] = res.emax
        if res.efin > 0.0:
            entry["efin"] = res.efin
        # PLEXOS Water Value ($/GWh) — terminal opportunity cost of
        # stored water.  Map to gtopt's ``efin + efin_cost``:
        # ``efin`` becomes the target end-of-horizon volume (eini, so
        # the LP is rewarded for keeping at least the initial volume)
        # and ``efin_cost`` is the per-hm³ shortfall penalty.  Unit
        # conversion: $/GWh × 1 GWh / 1000 MWh × mean_production_factor
        # [MWh/hm³] = $/hm³.  gtopt's default mean_production_factor
        # is 5 MWh/hm³ (reservoir.hpp:96) so the multiplier is 5/1000 =
        # 0.005.  For PLEXOS default 10,000 $/GWh, efin_cost = 50 $/hm³.
        #
        # PLEXOS "never drain" sentinel (Water Value 1e+30) lands here
        # as ``never_drain=True`` (water_value cleared to 0 by the
        # parser).  Emit a HARD ``efin = eini`` constraint with no
        # ``efin_cost`` slack — the LP must keep at least the initial
        # volume, no buy-out at any finite price.
        if res.never_drain and res.eini > 0.0 and "efin" not in entry:
            entry["efin"] = res.eini
        elif res.water_value > 0.0 and res.eini > 0.0 and "efin" not in entry:
            entry["efin"] = res.eini
            entry["efin_cost"] = res.water_value * 0.005
        # Activate gtopt's internal-drain mechanism on every reservoir.
        # storage_lp.hpp gates the drain column on ``drain_cost.has_value()``
        # and defaults the per-block upper bound to ``DblMax`` when
        # capacity is unset — i.e. setting just the cost gives an
        # unbounded "spill-to-sea" valve, no separate spillway_capacity
        # field needed.
        if res.spill_penalty_per_mwh > 0.0:
            # Reserved branch — currently never taken because the
            # PLEXOS extractor leaves ``spill_penalty_per_mwh`` at 0
            # (PLEXOS Storage "Spill Penalty" property is unset across
            # CEN PCP).  When the parser starts reading it, the
            # conversion is: gtopt ``spillway_cost`` is per-(m³/s)/h,
            # PLEXOS reports per-MWh — multiply by the global default
            # productibility (DESIGN.md §6).
            entry["spillway_cost"] = res.spill_penalty_per_mwh * DEFAULT_FP_MED
        else:
            # Default branch (taken for ALL reservoirs in CEN PCP v0).
            # 1000 is high enough that the LP only spills as a last
            # resort (e.g. CANUTILLAR with natural inflow but no
            # downstream Waterway).
            entry["spillway_cost"] = 1000.0
        out.append(entry)
    return out


def build_junction_array(
    junctions: tuple[JunctionSpec, ...],
) -> list[dict[str, Any]]:
    """One Junction per :class:`JunctionSpec`."""
    out: list[dict[str, Any]] = []
    for i, j in enumerate(junctions):
        entry: dict[str, Any] = {"uid": i + 1, "name": j.name}
        if j.drain:
            entry["drain"] = True
        out.append(entry)
    return out


def build_waterway_array(
    waterways: tuple[WaterwaySpec, ...],
) -> list[dict[str, Any]]:
    """One Waterway per :class:`WaterwaySpec` with both endpoints valid.

    PLEXOS Storage From / Storage To names map directly to gtopt's
    ``junction_a`` / ``junction_b`` since our junction naming mirrors
    the source Storage names.
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
) -> list[dict[str, Any]]:
    """One Turbine per :class:`TurbineSpec`.

    Each entry links a Generator (electrical output) to its upstream
    Reservoir (water balance via ``main_reservoir``).  When a Waterway
    is registered with ``storage_from`` matching the turbine's main
    reservoir, we set ``waterway`` to that Waterway — gtopt needs
    either ``waterway`` or ``flow`` set, otherwise it emits
    ``Turbine uid=…: no waterway or flow reference`` and the turbine's
    LP rows are empty.

    Turbines without a matching waterway are still emitted with just
    ``main_reservoir`` (gtopt logs a warning but does not fail).  v0
    does not synthesise a downstream Waterway when PLEXOS omits one.
    """
    ww_by_from: dict[str, str] = {}
    for w in waterways:
        if w.storage_from and w.storage_from not in ww_by_from:
            ww_by_from[w.storage_from] = w.name
    out: list[dict[str, Any]] = []
    skipped: list[str] = []
    for t in turbines:
        ww = ww_by_from.get(t.reservoir_name)
        if ww is None:
            # gtopt's planning validator rejects a Turbine without
            # either a Waterway or a Flow reference (no way to drive
            # the water-to-power conversion).  PLEXOS terminal hydro
            # plants (CANUTILLAR, RAPEL, ANGOSTURA U1/U2/U3) plus the
            # thermal false-turbine artefacts (ATA-TG_GNL_*, ...) all
            # land here.  Drop them with a single summary log line
            # rather than emit invalid entries.
            skipped.append(t.generator_name)
            continue
        entry: dict[str, Any] = {
            "uid": len(out) + 1,
            "name": f"turbine_{t.generator_name}",
            "generator": t.generator_name,
            "main_reservoir": t.reservoir_name,
            "waterway": ww,
        }
        if t.production_factor > 0.0:
            entry["production_factor"] = t.production_factor
        out.append(entry)
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


def build_flow_array(flows: tuple[FlowSpec, ...]) -> list[dict[str, Any]]:
    """One Flow per :class:`FlowSpec`, broadcasting the 24-block profile.

    gtopt's ``Flow.discharge`` is a per-(scene, stage, block) shape.
    For the daily PCP horizon we emit a single (1×1×24) matrix.
    """
    out: list[dict[str, Any]] = []
    for i, f in enumerate(flows):
        out.append(
            {
                "uid": i + 1,
                "name": f.name,
                "junction": f.junction_name,
                "discharge": [[list(f.discharge_profile)]],
            }
        )
    return out


def build_reserve_zone_array(
    reserves: tuple[ReserveSpec, ...],
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
    """
    zero_profile = [0.0] * DEFAULT_BLOCK_COUNT
    out: list[dict[str, Any]] = []
    for i, rsv in enumerate(reserves):
        entry: dict[str, Any] = {"uid": i + 1, "name": rsv.name}
        entry["urreq"] = (
            [list(rsv.ur_requirement)] if rsv.ur_requirement else [zero_profile]
        )
        entry["drreq"] = (
            [list(rsv.dr_requirement)] if rsv.dr_requirement else [zero_profile]
        )
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
    """
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
        # PLEXOS contingency / N-1 security rows are translated with
        # ``active = False`` by the parser (see
        # ``_is_contingency_constraint``).  Pass through; gtopt's
        # ``UserConstraintLP::add_to_lp`` gates on ``is_active(stage)``
        # so the row ships in the JSON but the monolithic LP skips
        # it.  ``None`` ⇒ leave the field unset ⇒ gtopt default
        # (active).
        # UserConstraint.active is `json_bool_null<OptBool>` (true/false),
        # NOT `json_number_null<OptBool>` (0/1) — see
        # include/gtopt/json/json_user_constraint.hpp.
        if c.active is not None:
            entry["active"] = bool(c.active)
        out.append(entry)
    return out


def build_flow_right_array(
    flow_rights: tuple[FlowRightSpec, ...],
) -> list[dict[str, Any]]:
    """One ``FlowRight`` per :class:`FlowRightSpec` with a resolved junction.

    Specs whose ``junction_name`` is None are dropped — gtopt's
    FlowRight requires a junction reference to apply. The drop is
    logged once when the parser resolves them, so the writer stays
    quiet.
    """
    out: list[dict[str, Any]] = []
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
        # fcost is OptTBRealFieldSched (same shape as Demand.fcost):
        # broadcast the scalar across the 24-block horizon as a
        # [[stage_blocks]] matrix when set.
        if fr.fcost > 0.0:
            entry["fcost"] = [[fr.fcost] * DEFAULT_BLOCK_COUNT]
        out.append(entry)
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
        # urmin / drmin emission: gtopt's ``commitment_lp.cpp`` (lines
        # 585-640) transforms the hard col-lowb into the conditional
        # row ``provision - urmin·u >= 0`` when the gen has a
        # Commitment.  Mirror PLEXOS "Min Provision × Available Units"
        # gating.  Drop when urmin > urmax (PLEXOS data inconsistency
        # — gen pmax too small to honour the floor; e.g. AGUAS_BLANCAS
        # at 1.827 MW vs 10 MW floor → drop, LP leaves provision in
        # [0, urmax]).
        urmin_eff = p.urmin
        drmin_eff = p.drmin
        if urmin_eff > 0.0 and p.urmax > 0.0 and urmin_eff > p.urmax:
            logger.warning(
                "reserve_provision %s: urmin (%.3f MW) > urmax "
                "(%.3f MW) — PLEXOS Min Provision exceeds gen pmax; "
                "dropping urmin so the LP leaves provision in [0, urmax].",
                entry["name"],
                urmin_eff,
                p.urmax,
            )
            urmin_eff = 0.0
        if drmin_eff > 0.0 and p.drmax > 0.0 and drmin_eff > p.drmax:
            logger.warning(
                "reserve_provision %s: drmin (%.3f MW) > drmax "
                "(%.3f MW) — dropping drmin.",
                entry["name"],
                drmin_eff,
                p.drmax,
            )
            drmin_eff = 0.0
        if urmin_eff > 0.0:
            entry["urmin"] = urmin_eff
        if drmin_eff > 0.0:
            entry["drmin"] = drmin_eff
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
        ),
        "line_array": build_line_array(case.lines),
        "demand_array": build_demand_array(case.demands),
        "battery_array": build_battery_array(case.batteries),
        "fuel_array": fuel_array,
        "junction_array": build_junction_array(case.junctions),
        "reservoir_array": build_reservoir_array(case.reservoirs),
        "waterway_array": build_waterway_array(case.waterways),
        "turbine_array": build_turbine_array(case.turbines, case.waterways),
        "flow_array": build_flow_array(case.flows),
        "reserve_zone_array": build_reserve_zone_array(case.reserves),
        "reserve_provision_array": build_reserve_provision_array(
            case.reserve_provisions
        ),
        "commitment_array": build_commitment_array(case.commitments),
        "flow_right_array": build_flow_right_array(case.flow_rights),
        "decision_variable_array": build_decision_variable_array(
            case.decision_variables
        ),
        "user_constraint_array": build_user_constraint_array(
            case.user_constraints, default_penalty=default_uc_penalty
        ),
    }
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
