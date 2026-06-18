"""PLEXOS ↔ gtopt conversion-correctness comparison and indicators.

Mirrors :mod:`plp2gtopt._comparison` for the PLEXOS converter, but covers the
many extra element classes plexos2gtopt produces (fuels, commitments,
reserves, decision variables, flow rights, FCF boundary cuts) and — the
centerpiece — verifies the **number and family (type)** of detected user
constraints end-to-end.

The architectural contract: the gtopt-JSON side is always produced by
:func:`gtopt_check_json._info.compute_counts` / ``compute_indicators`` (the
single authority), and the PLEXOS side is read from the parsed
:class:`~plexos2gtopt.entities.PlexosCase`.  The report then lines the two up:

* **Element Comparison** — every converted entity class, PLEXOS vs gtopt vs Δ.
* **User Constraint Families** — the conversion funnel (raw → dropped →
  synthesised → detected → emitted) plus a per-family PLEXOS-vs-gtopt table.
* **Global Indicators** — capacity / demand / energy / water / adequacy.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

from .entities import PlexosCase, generator_has_fuel_cost
from .uc_families import UC_FAMILY_ORDER, uc_family

logger = logging.getLogger(__name__)


def _profile_peak(
    scalar: float,
    profile: tuple[float, ...] | list[float] | None,
    layout: tuple[tuple[int, ...], ...] = (),
    reducer: str = "mean",
) -> float:
    """PLEXOS-side PEAK of a per-block *profile*, mirroring the writer.

    The writer emits time-varying caps/floors (``urmax``/``drmax`` reserve
    capability, fixed-load ``pmin``) as per-block profiles, aggregating the raw
    hourly series to blocks with *reducer* (``min`` for reserve caps, ``mean``
    for fixed load).  The gtopt side reads that emitted profile with
    :func:`_peak_scalar`, so to tie out the PLEXOS side must aggregate the raw
    profile the SAME way and then take the peak.  Falls back to *scalar* when no
    profile is present (the writer emits the scalar field in that case).
    """
    if profile:
        from .gtopt_writer import _aggregate_to_blocks  # noqa: PLC0415

        blocks = _aggregate_to_blocks(list(profile), layout, reducer=reducer)
        nums = [float(v) for v in blocks if isinstance(v, (int, float))]
        return max(nums) if nums else 0.0
    return float(scalar or 0.0)


def _plexos_effective_gen_pmin(
    gen: Any, layout: tuple[tuple[int, ...], ...] = ()
) -> float:
    """PEAK of the ``pmin`` the writer actually EMITS for *gen*.

    Mirrors :func:`plexos2gtopt.gtopt_writer.build_generator_array`: the
    always-on floor ``gen.pmin`` plus, for a fuel-cost unit with a Fixed Load,
    the forced ``pmin = pmax = fixed_load`` (curtailable renewables/RoR keep
    ``pmin = 0``).  Reducing by peak ties out with the gtopt-side
    ``_peak_scalar(generator.pmin)``.  This is the unconditional Fixed-Load
    floor only — the commitment-conditional Min Stable Level travels on
    ``Commitment.pmin`` (see ``commitment_min_stable_mw``).
    """
    base = float(gen.pmin) if gen.pmin and gen.pmin > 0.0 else 0.0
    if gen.fixed_load_profile and generator_has_fuel_cost(gen):
        peak_fl = _profile_peak(0.0, gen.fixed_load_profile, layout, reducer="mean")
        return max(base, peak_fl)
    return base


# UC families that are converter-synthesised wiring, NOT translated PLEXOS
# Constraint objects, so they are excluded from the PLEXOS↔gtopt user-
# constraint count comparison (they have no PLEXOS source to compare against).
# ``terminal_value`` is the single FCF / SDDP terminal-value cut the converter
# emits from ``Hydro_StoWaterValues.csv`` (the ``alpha_fcf`` row) — it lives in
# the Conversion Results base table, not in the comparison.
_COMPARISON_EXCLUDED_FAMILIES = frozenset({"terminal_value"})


# Every PLEXOS input file the converter reads, mapped to the report indicator
# that reflects it.  This is the "did we read it" ledger: each row's linked
# indicator goes non-zero only when the file was actually parsed, so a zero in
# the Global Indicators / element tables flags a file that was missing or
# silently skipped.  Keep in sync with the parsers in ``parsers.py`` /
# ``plexos_block_layout.py`` (read-sites cited in the converter docs).
# Columns: (file, what it carries, linked indicator).
_INPUT_FILE_INDICATORS: tuple[tuple[str, str, str], ...] = (
    # -- main database --
    (
        "DBSEN_PRGDIARIO.xml",
        "all objects/memberships/properties",
        "every element count",
    ),
    # -- generation --
    ("Gen_Rating.csv", "Generator.pmax (+ DLR profile)", "gen capacity (MW)"),
    ("Gen_UnitsOut.csv", "pmax derate by units-out", "gen capacity (MW)"),
    (
        "Gen_FixedLoad.csv",
        "forced pmin=pmax (fixed output)",
        "gen fixed-load floor (Σ MW)",
    ),
    (
        "Gen_MinStableLevel.csv",
        "Commitment.pmin (when-committed)",
        "commitment min-stable (Σ MW)",
    ),
    ("Gen_HeatRate.csv", "heat rate (+ segments)", "avg heat rate (fuel/MWh)"),
    ("Gen_VOMCharge.csv", "VOM charge (folded → gcost)", "avg/min/max gen cost"),
    ("Gen_AuxUse.csv", "aux-use fraction (folded → gcost)", "avg/min/max gen cost"),
    (
        "Gen_FuelTransportCharge.csv",
        "fuel transport (folded → gcost)",
        "avg/min/max gen cost",
    ),
    ("Gen_IniGeneration.csv", "Commitment.initial_power", "initial gen power (Σ MW)"),
    ("Gen_IniUnits.csv", "Commitment.initial_status", "units initially on (#)"),
    ("Gen_IniHoursUp.csv", "Commitment.initial_hours (+)", "initial commit hours (Σ)"),
    (
        "Gen_IniHoursDown.csv",
        "Commitment.initial_hours (−)",
        "initial commit hours (Σ)",
    ),
    ("Gen_Commit.csv", "commit gating (relax/forced)", "committable units (#)"),
    ("Gen_StartCost.csv", "Commitment.startup_cost", "total startup cost ($M)"),
    ("Gen_ShutDownCost.csv", "Commitment.shutdown_cost", "total shutdown cost ($M)"),
    ("CPF_<gen>_MRU/MRD.csv", "Commitment.ramp_up/down", "units w/ ramp limit (#)"),
    # -- network & demand --
    ("Lin_MaxRating.csv", "Line.tmax (+ DLR profile)", "line capacity (MW)"),
    ("Lin_MinRating.csv", "Line reverse rating", "line capacity (MW)"),
    ("Lin_Units.csv", "Line parallel circuits", "line capacity (MW)"),
    ("Nod_Load.csv", "Demand per-block lmax", "first/last/peak demand, total energy"),
    # -- storage --
    ("BESS_IniValue.csv", "Battery.eini (initial SoC)", "battery initial energy (MWh)"),
    # -- hydro --
    ("Hydro_MaxVolume.csv", "Reservoir.emax", "reservoir storage (Σ)"),
    ("Hydro_MinVolume.csv", "Reservoir.emin", "reservoir min vol (Σ)"),
    (
        "Hydro_InitialVolume.csv",
        "Reservoir.eini/efin",
        "reservoir initial/final vol (Σ)",
    ),
    (
        "Hydro_WaterFlows.csv",
        "inflows + forced flows",
        "first block flow / total water vol",
    ),
    ("Hydro_EfficiencyIncr.csv", "turbine production factor", "turbine capacity (MW)"),
    (
        "Hydro_StoWaterValues.csv",
        "FCF future-cost slopes",
        "FCF intercept / water-value reservoirs",
    ),
    (
        "Hydro_AntucoBounds.csv",
        "hydro discharge bounds (UCs)",
        "user constraints (operational)",
    ),
    (
        "Hydro_MaxRampDay.csv",
        "hydro daily-ramp (UCs)",
        "user constraints (operational)",
    ),
    # -- reserves --
    ("Res_Requirement.csv", "reserve zone requirement", "up/down reserve req (MW)"),
    ("Res_Timeslice.csv", "reserve day-type selection", "up/down reserve req (MW)"),
    (
        "SSCC_Activation_BESS.csv",
        "BESS ancillary activation %",
        "up/dn reserve provision (MW)",
    ),
    # -- fuels --
    ("Fuel_Price.csv", "Fuel.price", "avg fuel price ($/unit)"),
    ("Fuel_MaxOfftakeWeek.csv", "Fuel.max_offtake (weekly)", "fuel offtake cap (Σ)"),
    # -- solution database (optional) --
    ("<Model> Solution.accdb (t_phase_3)", "PLEXOS block layout", "blocks (#)"),
)


def _comparable_total(by_family: dict[str, int], total: int) -> int:
    """Total user constraints minus the converter-synthesised families."""
    return total - sum(by_family.get(f, 0) for f in _COMPARISON_EXCLUDED_FAMILIES)


# ---------------------------------------------------------------------------
# PLEXOS-side generator classification (mirrors gtopt_writer's type rule so
# the capacity breakdown lines up with the gtopt JSON ``type`` field).
# ---------------------------------------------------------------------------


def _turbine_gen_names(case: PlexosCase) -> frozenset[str]:
    """Names of generators that drive a hydro turbine."""
    return frozenset(t.generator_name for t in case.turbines if t.generator_name)


def _gen_is_thermal(gen: Any) -> bool:
    """Replicate ``gtopt_writer``'s thermal test (has fuel / heat rate)."""
    return bool(
        getattr(gen, "fuel_names", None)
        or getattr(gen, "heat_rate", 0.0) > 0.0
        or getattr(gen, "fuel_price_override", 0.0) > 0.0
    )


def _effective_pmax(gen: Any) -> float:
    """Nameplate capacity: scalar ``pmax`` else the profile peak.

    Matches the gtopt-side :func:`compute_indicators`, which reads ``capacity``
    via ``_peak_scalar`` (profile peak = installed nameplate), so the capacity
    indicator compares nameplate-to-nameplate rather than against a renewable's
    near-zero first hour.
    """
    pmax = float(getattr(gen, "pmax", 0.0) or 0.0)
    if pmax > 0.0:
        return pmax
    profile = getattr(gen, "pmax_profile", ()) or ()
    return max((float(v) for v in profile), default=0.0)


# ---------------------------------------------------------------------------
# Element counts
# ---------------------------------------------------------------------------


def _plexos_element_counts(case: PlexosCase) -> dict[str, Any]:
    """Extract element counts from the parsed :class:`PlexosCase`.

    Pure reads — no database re-parse.  Generators are also bucketed into
    hydro / thermal / renewable using the same rule the writer applies when
    emitting the ``type`` field, so the breakdown matches the gtopt side.
    """
    # FCF boundary slopes split into in-bundle (emitted) vs discarded (water
    # value for a non-bundle reservoir like PILMAIQUEN, dropped by the writer).
    _res_names = {r.name for r in case.reservoirs}
    _bsv_slopes = case.boundary_cut.slopes if case.boundary_cut is not None else {}
    _bsv_in_bundle = sum(1 for r in _bsv_slopes if r in _res_names)
    _bsv_discarded = sorted(r for r in _bsv_slopes if r not in _res_names)

    turbine_gens = _turbine_gen_names(case)
    gen_hydro = gen_thermal = gen_renewable = 0
    for g in case.generators:
        if g.name in turbine_gens:
            gen_hydro += 1
        elif _gen_is_thermal(g):
            gen_thermal += 1
        else:
            gen_renewable += 1

    return {
        "buses": len(case.nodes),
        "generators": len(case.generators),
        "gen_hydro": gen_hydro,
        "gen_thermal": gen_thermal,
        "gen_renewable": gen_renewable,
        "lines": len(case.lines),
        "demands": len(case.demands),
        "batteries": len(case.batteries),
        "fuels": len(case.fuels),
        "reservoirs": len(case.reservoirs),
        "waterways": len(case.waterways),
        "junctions": len(case.junctions),
        "turbines": len(case.turbines),
        "flows": len(case.flows),
        "reserve_zones": len(case.reserves),
        "reserve_provisions": len(case.reserve_provisions),
        "commitments": len(case.commitments),
        "flow_rights": len(case.flow_rights),
        "decision_variables": len(case.decision_variables),
        "user_constraints": len(case.user_constraints),
        "boundary_cuts": 1 if case.boundary_cut is not None else 0,
        # Count only boundary slopes that map to a bundle reservoir — the writer
        # can only attach a slope to a reservoir that exists in the model, so a
        # water value for a non-bundle reservoir (e.g. PILMAIQUEN) is out of
        # scope for the comparison (named in the row note, not counted as a Δ).
        # Mirrors ``num_water_value_reservoirs`` so both rows tie out.
        "boundary_state_variables": _bsv_in_bundle,
        "boundary_state_variables_discarded": _bsv_discarded,
    }


def _gtopt_element_counts(planning: dict[str, Any], base_dir: str | None = None):
    """Return the gtopt-JSON element counts via the gtopt_check authority.

    Delegates to :func:`gtopt_check_json._info.compute_counts` — never
    re-derives counts locally (honours the "let gtopt_check generate the
    stats over the JSON" contract).
    """
    from gtopt_check_json._info import compute_counts  # noqa: PLC0415

    return compute_counts(planning, base_dir=base_dir)


# ---------------------------------------------------------------------------
# User-constraint classification (the centerpiece)
# ---------------------------------------------------------------------------


def _plexos_uc_classification(case: PlexosCase) -> dict[str, Any]:
    """Classify the case's user constraints by family + active state.

    Returns a dict with ``by_family`` (family → count), ``active``,
    ``inactive`` and ``total`` over ``case.user_constraints``, plus the
    conversion funnel from ``case.uc_stats`` (raw → dropped → synthesised).
    """
    by_family: dict[str, int] = {}
    active = inactive = 0
    for uc in case.user_constraints:
        fam = uc_family(uc.name)
        by_family[fam] = by_family.get(fam, 0) + 1
        if uc.active is False:
            inactive += 1
        else:
            active += 1

    stats = case.uc_stats
    return {
        "by_family": by_family,
        "active": active,
        "inactive": inactive,
        "total": len(case.user_constraints),
        "raw_plexos_constraints": getattr(stats, "raw_plexos_constraints", 0),
        "empty_lhs_dropped": getattr(stats, "empty_lhs_dropped", 0),
        "base_emitted": getattr(stats, "base_emitted", 0),
        "hydro_synthesized": getattr(stats, "hydro_synthesized", 0),
        "plant_cap_synthesized": getattr(stats, "plant_cap_synthesized", 0),
    }


# ---------------------------------------------------------------------------
# Global indicators
# ---------------------------------------------------------------------------


def _plexos_indicators(case: PlexosCase) -> dict[str, float]:
    """Compute aggregate PLEXOS-side indicators from the parsed case.

    Mirrors the gtopt-side :func:`compute_indicators` fields so the two can
    be shown side by side.  Capacity is bucketed with the same hydro /
    thermal / renewable rule used for the counts; demand / energy / water are
    summed across the per-block profiles.
    """
    # Block layout (interval-id lists per block) drives the same per-block
    # aggregation the writer applies, so PEAK-based PLEXOS-side indicators tie
    # out with the gtopt side's _peak_scalar of the emitted (aggregated) field.
    layout = getattr(case.bundle, "block_layout", ()) or ()
    turbine_gens = _turbine_gen_names(case)
    total_cap = hydro_cap = thermal_cap = renewable_cap = 0.0
    for g in case.generators:
        cap = _effective_pmax(g)
        total_cap += cap
        if g.name in turbine_gens:
            hydro_cap += cap
        elif _gen_is_thermal(g):
            thermal_cap += cap
        else:
            renewable_cap += cap
    # Battery discharge capacity counts as generation, mirroring the gtopt
    # side (batteries become auxiliary generators at LP build time).
    for bat in case.batteries:
        total_cap += float(getattr(bat, "pmax_discharge", 0.0) or 0.0)

    # Line capacity: NOMINAL bidirectional rating, mirroring the writer
    # (``gtopt_writer.build_line_array``): forward = ``tmax_ab``, reverse =
    # ``|tmin_ab|`` (falling back to ``tmax_ab``), each × ``units``, and ONLY
    # for lines that carry a cap (``tmax_ab > 0`` — the writer emits no tmax
    # otherwise).  Matches the gtopt side's Σ(tmax_normal_ab + tmax_normal_ba)
    # — the pre-soft-cap nominal — rather than the inflated LP hard cap.
    line_cap = 0.0
    for ln in case.lines:
        if ln.tmax_ab <= 0.0:
            continue
        units = max(1, ln.units)
        # Use the scalar nominal (= profile PEAK for dynamic-rating lines),
        # not the off-peak first hour, so the rating matches the gtopt side.
        fwd = ln.tmax_ab
        rev = abs(ln.tmin_ab) if ln.tmin_ab else fwd
        line_cap += (abs(fwd) + abs(rev)) * units

    # Reserve requirement (MW): PEAK up (``ur_requirement``) and down
    # (``dr_requirement``) summed across all reserve zones — block 0 is the
    # off-peak (midnight) hour, so peak is the binding system requirement.
    up_reserve_req = sum(
        max(r.ur_requirement) for r in case.reserves if r.ur_requirement
    )
    dn_reserve_req = sum(
        max(r.dr_requirement) for r in case.reserves if r.dr_requirement
    )

    # Demand per block (sum across demands).
    block_demand: list[float] = []
    for dem in case.demands:
        prof = dem.lmax_profile or ()
        for i, v in enumerate(prof):
            if i >= len(block_demand):
                block_demand.append(0.0)
            block_demand[i] += float(v)
    first_dem = block_demand[0] if block_demand else 0.0
    last_dem = block_demand[-1] if block_demand else 0.0

    # Energy = Σ demand × duration.  The case profiles may be at hourly
    # resolution (one value per interval) or block resolution (one value per
    # block); ``_durations_for`` returns a matching per-entry duration vector
    # so the energy total stays consistent with the gtopt side regardless.
    total_energy = 0.0
    total_hours = 0.0
    for dem in case.demands:
        prof = dem.lmax_profile or ()
        durs = _durations_for(case, len(prof))
        total_hours = max(total_hours, sum(durs))
        for v, dur in zip(prof, durs):
            total_energy += float(v) * dur

    # Affluent (hydro inflow) first/last entry + water volume.
    first_afl = last_afl = 0.0
    total_water_hm3 = 0.0
    n_flow = 0
    for flow in case.flows:
        prof = flow.discharge_profile or ()
        if not prof:
            continue
        n_flow += 1
        first_afl += float(prof[0])
        last_afl += float(prof[-1])
        durs = _durations_for(case, len(prof))
        for v, dur in zip(prof, durs):
            total_water_hm3 += float(v) * dur * 0.0036
    avg_flow = (
        total_water_hm3 / (total_hours * 0.0036) / n_flow
        if n_flow and total_hours > 0.0
        else 0.0
    )

    # === Tier 1: cost & fuel ===  (mirror the writer's effective gcost =
    # fuel.price × heat_rate + VOM + transport; renewables/precosted → VOM)
    fuel_price = {f.name: f.price for f in case.fuels}
    gcosts: list[float] = []
    heat_rates: list[float] = []
    for g in case.generators:
        hr = g.heat_rate
        base = g.vom_charge + g.fuel_transport
        has_fuel = bool(g.fuel_names) or g.fuel_price_override > 0.0
        if has_fuel and hr > 0.0:
            price = (
                g.fuel_price_override
                if g.fuel_price_override > 0.0
                else fuel_price.get(g.fuel_names[0] if g.fuel_names else "", 0.0)
            )
            gcosts.append(price * hr + base)
        else:
            gcosts.append(base)
        if hr > 0.0:
            heat_rates.append(hr)
    fuel_caps = [f.max_offtake for f in case.fuels if f.max_offtake is not None]

    # === Tier 2: commitment ===
    total_startup = sum(c.startup_cost for c in case.commitments)
    total_shutdown = sum(c.shutdown_cost for c in case.commitments)
    num_committable = sum(1 for c in case.commitments if c.startup_cost > 0.0)

    # === Tier 3: storage & FCF ===
    bat_effs = [b.input_efficiency * b.output_efficiency for b in case.batteries]
    fcf = case.boundary_cut
    # Count only water values that map to a bundle reservoir — the writer can
    # only attach a slope to a reservoir that exists in the model, so a water
    # value for a non-bundle reservoir (e.g. PILMAIQUEN) is out of scope for
    # the comparison (named in the report note instead of counted as a drop).
    res_names = {r.name for r in case.reservoirs}
    wv_in_bundle = sum(1 for r in fcf.slopes if r in res_names) if fcf else 0

    # === Tier 4: network & demand ===
    pmax_by_name = {g.name: _effective_pmax(g) for g in case.generators}
    total_turb_cap = sum(pmax_by_name.get(t.generator_name, 0.0) for t in case.turbines)
    peak_dem = max(block_demand) if block_demand else 0.0
    renew_energy = 0.0
    for g in case.generators:
        if g.name in turbine_gens or _gen_is_thermal(g):
            continue
        prof = g.pmax_profile or ()
        if prof:
            for v, dur in zip(prof, _durations_for(case, len(prof))):
                renew_energy += float(v) * dur
        elif g.pmax > 0.0:
            renew_energy += g.pmax * total_hours

    # Reserve provision peak capability (mirrors the gtopt-side _peak_scalar of
    # the emitted urmax/drmax profile) — reused for the adequacy margin below.
    total_up_prov = sum(
        _profile_peak(p.urmax, p.urmax_profile, layout, reducer="min")
        for p in case.reserve_provisions
    )
    total_dn_prov = sum(
        _profile_peak(p.drmax, p.drmax_profile, layout, reducer="min")
        for p in case.reserve_provisions
    )

    # Fuels EXPECTED to carry an emission factor after the converter's
    # emission-defaults overlay.  PLEXOS ships no explicit rates for CEN PCP, so
    # this reuses the canonical ``gtopt_shared.emissions`` defaults (the same
    # bundled IPCC file the converter applies) to predict COVERAGE: a fuel is
    # covered when it already has an explicit rate OR a defaults entry with a
    # non-zero factor.  A Δ vs the emitted gtopt count flags a fuel family the
    # defaults do not cover (the real risk).  Rate VALUES are not compared —
    # they are synthesized, so re-deriving them independently is not robust.
    num_fuel_emission_factors = 0
    try:
        from gtopt_shared.emissions import load_emission_defaults  # noqa: PLC0415

        _defaults = load_emission_defaults()
        for fu in case.fuels:
            if fu.co2_rate or fu.co2_upstream_rate:
                num_fuel_emission_factors += 1
                continue
            factor = _defaults.lookup(fu.name, subtype_hint=(fu.subtype or None))
            if factor is not None and factor.has_factor:
                num_fuel_emission_factors += 1
    except (ImportError, FileNotFoundError, ValueError):
        num_fuel_emission_factors = sum(
            1 for fu in case.fuels if fu.co2_rate or fu.co2_upstream_rate
        )

    # Fixed-load forced energy (GWh): Σ emitted-equivalent pmin per block ×
    # duration (mirrors gtopt_writer.build_generator_array's fixed-load branch),
    # plus the always-on scalar floor × horizon.  Ties out with the gtopt-side
    # Σ generator.pmin × duration.
    from .gtopt_writer import _aggregate_to_blocks  # noqa: PLC0415

    fixed_load_energy = 0.0
    for g in case.generators:
        base = float(g.pmin) if g.pmin and g.pmin > 0.0 else 0.0
        if g.fixed_load_profile:
            fl_blocks = (
                _aggregate_to_blocks(list(g.fixed_load_profile), layout, reducer="mean")
                if layout
                else list(g.fixed_load_profile)
            )
            hf = generator_has_fuel_cost(g)
            blk = [(fl if hf else 0.0) if fl > 0.0 else base for fl in fl_blocks]
            for v, dur in zip(blk, _durations_for(case, len(blk))):
                fixed_load_energy += v * dur
        elif base > 0.0:
            fixed_load_energy += base * total_hours

    # Profile-coverage counts — # elements with a NON-CONSTANT source profile;
    # a "profile silently flattened to a scalar" regression shows up as a Δ vs
    # the gtopt side's count of non-constant emitted profiles.
    def _varying(prof: tuple[float, ...] | list[float] | None) -> bool:
        if not prof:
            return False
        nums = [float(v) for v in prof]
        return bool(nums) and (max(nums) - min(nums)) > 1e-9

    num_gen_pmax_profiles = sum(1 for g in case.generators if _varying(g.pmax_profile))
    num_provision_profiles = sum(
        1
        for p in case.reserve_provisions
        if _varying(p.urmax_profile) or _varying(p.drmax_profile)
    )

    return {
        "total_gen_capacity_mw": total_cap,
        "hydro_capacity_mw": hydro_cap,
        "thermal_capacity_mw": thermal_cap,
        "total_line_capacity_mw": line_cap,
        "first_block_demand_mw": first_dem,
        "last_block_demand_mw": last_dem,
        "total_energy_mwh": total_energy,
        "avg_annual_energy_mwh": (
            total_energy * 8760.0 / total_hours if total_hours > 0.0 else 0.0
        ),
        "first_block_affluent_avg": first_afl,
        "last_block_affluent_avg": last_afl,
        "total_water_volume_hm3": total_water_hm3,
        "avg_flow_m3s": avg_flow,
        "up_reserve_req_mw": up_reserve_req,
        "dn_reserve_req_mw": dn_reserve_req,
        # Tier 1: cost & fuel
        "avg_gcost": sum(gcosts) / len(gcosts) if gcosts else 0.0,
        "min_gcost": min(gcosts) if gcosts else 0.0,
        "max_gcost": max(gcosts) if gcosts else 0.0,
        "avg_heat_rate": sum(heat_rates) / len(heat_rates) if heat_rates else 0.0,
        "avg_fuel_price": (
            sum(p for p in fuel_price.values() if p > 0.0)
            / len([p for p in fuel_price.values() if p > 0.0])
            if any(p > 0.0 for p in fuel_price.values())
            else 0.0
        ),
        "total_fuel_offtake_cap": sum(fuel_caps),
        "num_fuel_caps": float(len(fuel_caps)),
        # Tier 2: commitment
        "total_startup_cost": total_startup,
        "total_shutdown_cost": total_shutdown,
        "num_committable": float(num_committable),
        # Tier 3: storage & FCF
        "total_battery_energy_mwh": sum(b.emax for b in case.batteries),
        "total_battery_power_mw": sum(b.pmax_discharge for b in case.batteries),
        "avg_battery_efficiency": (sum(bat_effs) / len(bat_effs) if bat_effs else 0.0),
        "total_reservoir_storage": sum(r.emax for r in case.reservoirs),
        "total_reservoir_initial": sum(r.eini for r in case.reservoirs),
        "total_reservoir_final": sum(r.efin for r in case.reservoirs),
        "fcf_intercept": fcf.fcf if fcf else 0.0,
        "num_water_value_reservoirs": float(wv_in_bundle),
        # Tier 4: network & demand
        "num_lossy_lines": float(sum(1 for ln in case.lines if ln.resistance > 0.0)),
        "total_turbine_capacity_mw": total_turb_cap,
        # Reserve provision caps: PEAK of the per-(gen, hour) CFdata MRU/MRD
        # reserve-capability profile (writer aggregates with reducer="min"),
        # NOT the scalar ``urmax = pmax`` nameplate fallback — that fallback is
        # 2-16000x too loose and is only emitted when no profile exists.  Mirror
        # the writer so this ties out with the gtopt-side _peak_scalar(urmax).
        "total_up_provision_mw": total_up_prov,
        "total_dn_provision_mw": total_dn_prov,
        "up_reserve_margin_mw": total_up_prov - up_reserve_req,
        "dn_reserve_margin_mw": total_dn_prov - dn_reserve_req,
        "num_fuel_emission_factors": float(num_fuel_emission_factors),
        "fixed_load_energy_gwh": fixed_load_energy / 1e3,
        "num_gen_pmax_profiles": float(num_gen_pmax_profiles),
        "num_provision_profiles": float(num_provision_profiles),
        "peak_demand_mw": peak_dem,
        "demand_fail_cost": float(getattr(case.bundle, "demand_fail_cost", 0.0) or 0.0),
        "renewable_energy_gwh": renew_energy / 1e3,
        # Tier 5: input-file read receipts (one aggregate per otherwise
        # indicator-less PLEXOS input file — see the input-file table in the
        # report).  These tie out 1:1 with the gtopt side because the writer
        # copies each spec field straight into the JSON.
        # Fixed-Load forced floor (unconditional pmin=pmax) — the writer derives
        # the emitted generator.pmin from fixed_load_profile, NOT the raw
        # ``g.pmin`` (which is 0 in CEN PCP: PLEXOS keeps Min Stable Level on the
        # Commitment object).  Mirror the writer so this ties out.
        "total_gen_min_stable_mw": sum(
            _plexos_effective_gen_pmin(g, layout) for g in case.generators
        ),
        # Commitment-conditional Min Stable Level (Gen_MinStableLevel.csv →
        # Commitment.pmin, enforced only when the unit is committed).  Distinct
        # from the unconditional Fixed-Load floor above; surfaced separately so
        # the audit does not silently drop PLEXOS's primary min-generation data.
        "commitment_min_stable_mw": sum(
            _profile_peak(c.pmin, getattr(c, "pmin_profile", ()), layout)
            for c in case.commitments
        ),
        "total_initial_gen_power_mw": sum(c.initial_power for c in case.commitments),
        "num_units_initial_on": float(
            sum(1 for c in case.commitments if c.initial_status > 0)
        ),
        "total_initial_commit_hours": sum(
            abs(c.initial_hours) for c in case.commitments
        ),
        "num_units_with_ramp_limit": float(
            sum(1 for c in case.commitments if c.ramp_up > 0.0)
        ),
        "total_battery_initial_mwh": sum(b.eini for b in case.batteries),
        "total_reservoir_min_vol": sum(r.emin for r in case.reservoirs),
    }


def _durations_for(case: PlexosCase, n: int) -> list[float]:
    """Per-entry durations (hours) for a profile of length *n*.

    A PLEXOS ``block_layout`` groups interval ids per block; a block's
    duration is its interval count.  A case profile may be stored at either
    resolution, so pick the matching duration vector:

    * ``n == num_blocks``  → block durations ``[len(blk) for blk in layout]``;
    * ``n == total_intervals`` (or no layout) → a uniform 1-hour grid.

    Both sum to the same horizon hours, keeping energy/water totals
    consistent with the gtopt side whichever resolution the case used.
    """
    layout = getattr(case.bundle, "block_layout", ()) or ()
    if layout and n:
        if n == len(layout):
            return [float(len(blk)) for blk in layout]
        total_iv = sum(len(blk) for blk in layout)
        # ``n`` evenly divides the horizon → uniform weight ``total_iv / n``
        # per entry.  Covers the full-hourly case (factor 1) and the daily
        # pattern tiled across ``n_days`` days (factor = n_days, e.g. 24→168).
        if total_iv and total_iv % n == 0:
            return [float(total_iv // n)] * n
    return [1.0] * n


def _gtopt_indicators(
    planning: dict[str, Any], base_dir: str | None = None
) -> dict[str, float]:
    """gtopt-side indicators via :func:`compute_indicators` (the authority)."""
    from gtopt_check_json._info import compute_indicators  # noqa: PLC0415

    ind = compute_indicators(planning, base_dir=base_dir)
    return {
        "total_gen_capacity_mw": ind.total_gen_capacity_mw,
        "hydro_capacity_mw": ind.hydro_capacity_mw,
        "thermal_capacity_mw": ind.thermal_capacity_mw,
        "total_line_capacity_mw": ind.total_line_capacity_mw,
        "first_block_demand_mw": ind.first_block_demand_mw,
        "last_block_demand_mw": ind.last_block_demand_mw,
        "total_energy_mwh": ind.total_energy_mwh,
        "avg_annual_energy_mwh": ind.avg_annual_energy_mwh,
        "first_block_affluent_avg": ind.first_block_affluent_avg,
        "last_block_affluent_avg": ind.last_block_affluent_avg,
        "total_water_volume_hm3": ind.total_water_volume_hm3,
        "avg_flow_m3s": ind.avg_flow_m3s,
        "up_reserve_req_mw": ind.up_reserve_req_mw,
        "dn_reserve_req_mw": ind.dn_reserve_req_mw,
        "avg_gcost": ind.avg_gcost,
        "min_gcost": ind.min_gcost,
        "max_gcost": ind.max_gcost,
        "avg_heat_rate": ind.avg_heat_rate,
        "avg_fuel_price": ind.avg_fuel_price,
        "total_fuel_offtake_cap": ind.total_fuel_offtake_cap,
        "num_fuel_caps": float(ind.num_fuel_caps),
        "total_startup_cost": ind.total_startup_cost,
        "total_shutdown_cost": ind.total_shutdown_cost,
        "num_committable": float(ind.num_committable),
        "total_battery_energy_mwh": ind.total_battery_energy_mwh,
        "total_battery_power_mw": ind.total_battery_power_mw,
        "avg_battery_efficiency": ind.avg_battery_efficiency,
        "total_reservoir_storage": ind.total_reservoir_storage,
        "total_reservoir_initial": ind.total_reservoir_initial,
        "total_reservoir_final": ind.total_reservoir_final,
        "fcf_intercept": ind.fcf_intercept,
        "num_water_value_reservoirs": float(ind.num_water_value_reservoirs),
        "num_lossy_lines": float(ind.num_lossy_lines),
        "total_turbine_capacity_mw": ind.total_turbine_capacity_mw,
        "total_up_provision_mw": ind.total_up_provision_mw,
        "total_dn_provision_mw": ind.total_dn_provision_mw,
        "up_reserve_margin_mw": ind.up_reserve_margin_mw,
        "dn_reserve_margin_mw": ind.dn_reserve_margin_mw,
        "num_fuel_emission_factors": float(ind.num_fuel_emission_factors),
        "fixed_load_energy_gwh": ind.fixed_load_energy_gwh,
        "num_gen_pmax_profiles": float(ind.num_gen_pmax_profiles),
        "num_provision_profiles": float(ind.num_provision_profiles),
        "peak_demand_mw": ind.peak_demand_mw,
        "demand_fail_cost": ind.demand_fail_cost,
        "renewable_energy_gwh": ind.renewable_energy_gwh,
        # Tier 5: input-file read receipts
        "total_gen_min_stable_mw": ind.total_gen_min_stable_mw,
        "commitment_min_stable_mw": ind.commitment_min_stable_mw,
        "total_initial_gen_power_mw": ind.total_initial_gen_power_mw,
        "num_units_initial_on": float(ind.num_units_initial_on),
        "total_initial_commit_hours": ind.total_initial_commit_hours,
        "num_units_with_ramp_limit": float(ind.num_units_with_ramp_limit),
        "total_battery_initial_mwh": ind.total_battery_initial_mwh,
        "total_reservoir_min_vol": ind.total_reservoir_min_vol,
    }


def compute_comparison_indicators(
    case: PlexosCase,
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> tuple[dict[str, float], dict[str, float]]:
    """Return ``(plexos_indicators, gtopt_indicators)`` for the report."""
    return _plexos_indicators(case), _gtopt_indicators(planning, base_dir=base_dir)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def _log_comparison(
    plexos_counts: dict[str, Any],
    gtopt_counts: Any,
    plexos_uc: dict[str, Any],
    plexos_ind: dict[str, float] | None = None,
    gtopt_ind: dict[str, float] | None = None,
    ind_notes: dict[str, str] | None = None,
) -> None:
    """Print the three side-by-side PLEXOS-vs-gtopt comparison tables.

    Output goes to stderr via :mod:`rich` with automatic colour / Unicode
    detection, reusing the gtopt_check_json terminal helpers so the styling
    matches the rest of the converter's output.
    """
    from gtopt_check_json._terminal import (  # noqa: PLC0415
        console,
        print_section,
        table_width,
    )
    from rich.box import ASCII, ROUNDED  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415

    con = console()
    colr = con.is_terminal
    box_style = ROUNDED if colr else ASCII
    tw = table_width()

    def _v(val: int | None) -> str:
        return str(val) if val is not None else ""

    def _delta(plexos: int | None, gtopt: int | None) -> str:
        if plexos is None or gtopt is None:
            return ""
        diff = gtopt - plexos
        if diff == 0:
            return "[bold green]✓[/bold green]" if colr else "ok"
        sign = "+" if diff > 0 else ""
        style = "[bold yellow]" if diff < 0 else "[bold cyan]"
        end = "[/bold yellow]" if diff < 0 else "[/bold cyan]"
        return f"{style}{sign}{diff}{end}" if colr else f"{sign}{diff}"

    # =====================================================================
    # Table 1 — Element comparison
    # =====================================================================
    print_section("PLEXOS vs gtopt Element Comparison")
    table = Table(
        box=box_style,
        show_lines=False,
        padding=(0, 1),
        title_justify="left",
        width=tw,
    )
    table.add_column("Element", no_wrap=True, min_width=26)
    table.add_column("PLEXOS", justify="right", min_width=7, no_wrap=True)
    table.add_column("gtopt", justify="right", min_width=7, no_wrap=True)
    table.add_column("Δ", justify="right", min_width=4, no_wrap=True)
    table.add_column("Notes", style="dim")

    def _row(
        label: str,
        plexos: int | None = None,
        gtopt: int | None = None,
        note: str = "",
        *,
        indent: int = 0,
        section: bool = False,
    ) -> None:
        lbl = ("  " * indent) + label
        if section:
            table.add_row(f"[bold]{lbl}[/bold]" if colr else lbl, "", "", "", "")
        else:
            table.add_row(lbl, _v(plexos), _v(gtopt), _delta(plexos, gtopt), note)

    g = gtopt_counts  # SystemCounts
    p = plexos_counts

    # -- Network & generation --
    _row("Network & Generation", section=True)
    _row("buses", p["buses"], g.buses)
    _row("lines", p["lines"], g.lines)
    _row("generators", p["generators"], g.generators)
    # gtopt's gen_by_type classifies turbine-driven units as hydro (see
    # compute_counts), so the hydro / renewable split lines up with PLEXOS.
    g_hydro = g.gen_by_type.get("hydro", 0) + g.gen_by_type.get("hydro_ror", 0)
    g_thermal = g.gen_by_type.get("thermal", 0)
    g_renew = g.gen_by_type.get("renewable", 0)
    _row("hydro", p["gen_hydro"], g_hydro or None, indent=1)
    _row("thermal", p["gen_thermal"], g_thermal or None, indent=1)
    _row("renewable", p["gen_renewable"], g_renew or None, indent=1)
    _row("demands", p["demands"], g.demands)

    # -- Storage --
    _row("Storage", section=True)
    _row("batteries", p["batteries"], g.batteries)

    # -- Hydro --
    _row("Hydro", section=True)
    _row("reservoirs", p["reservoirs"], g.reservoirs)
    _row("turbines", p["turbines"], g.turbines)
    # Turbines now carry their own flow arc (junction_a/junction_b) and convert
    # it to power, so the writer no longer synthesises per-turbine penstock
    # waterways or terminal-ocean sink junctions — current conversions tie out
    # 1:1 with PLEXOS.  The ``*_synth`` subtraction stays as a back-compat
    # safety net so an OLD JSON (with penstock_*/_terminal_ocean elements)
    # still reconciles instead of showing a spurious Δ.
    j_note = (
        f"+ {g.junctions_synth} synthesised terminal-ocean sinks"
        if g.junctions_synth
        else ""
    )
    _row("junctions", p["junctions"], g.junctions - g.junctions_synth, note=j_note)
    w_note = (
        f"+ {g.waterways_synth} synthesised per-turbine penstocks"
        if g.waterways_synth
        else ""
    )
    _row("waterways", p["waterways"], g.waterways - g.waterways_synth, note=w_note)
    _row("flows", p["flows"], g.flows)

    # -- Reserves --
    _row("Reserves", section=True)
    _row("reserve zones", p["reserve_zones"], g.reserve_zones)
    _row("reserve provisions", p["reserve_provisions"], g.reserve_provisions)

    # -- Fuels & commitment --
    _row("Fuels & Commitment", section=True)
    _row("fuels", p["fuels"], g.fuels)
    _row("commitments", p["commitments"], g.commitments)

    # -- Other LP classes --
    _row("Other", section=True)
    _row("flow rights", p["flow_rights"], g.flow_rights)
    # Compare PLEXOS-sourced decision variables only — the converter also adds
    # the synthesised ``alpha_fcf`` FCF cost-to-go column(s), noted separately.
    g_dv = g.decision_variables - g.decision_variables_alpha
    dv_note = (
        f"+ {g.decision_variables_alpha} synthesised alpha_fcf (FCF)"
        if g.decision_variables_alpha
        else ""
    )
    _row("decision variables", p["decision_variables"], g_dv, note=dv_note)
    _row("boundary cuts", p["boundary_cuts"], g.boundary_cuts)
    # PLEXOS side counts only in-bundle slopes (matching what the writer emits);
    # any water value for a non-bundle reservoir is named in the note so the
    # row ties out instead of showing a spurious Δ (same PILMAIQUEN case as the
    # "water-value reservoirs" indicator row).
    bsv_discarded = p.get("boundary_state_variables_discarded", [])
    bsv_note = "per-reservoir FCF slopes"
    if bsv_discarded:
        bsv_note += f"; discarded (not a bundle reservoir): {', '.join(bsv_discarded)}"
    _row(
        "boundary state vars",
        p["boundary_state_variables"],
        g.boundary_state_variables,
        note=bsv_note,
    )

    # -- User constraints (totals; family detail in Table 2).  Excludes the
    #    converter-synthesised terminal_value FCF cut so both sides compare
    #    PLEXOS-sourced constraints only. --
    p_uc_total = _comparable_total(plexos_uc["by_family"], plexos_uc["total"])
    g_uc_total = _comparable_total(g.uc_by_family, g.user_constraints_total)
    _row("User Constraints", section=True)
    _row("total", p_uc_total, g_uc_total)

    # -- Simulation --
    _row("Simulation", section=True)
    _row("blocks", None, g.num_blocks)
    _row("stages", None, g.num_stages)
    _row("scenarios", None, g.num_scenarios)

    con.print(table)

    # =====================================================================
    # Table 2 — User constraint families (centerpiece)
    # =====================================================================
    _log_uc_families(plexos_uc, gtopt_counts, con, box_style, tw, colr)

    # =====================================================================
    # Table 3 — Global indicators
    # =====================================================================
    if plexos_ind and gtopt_ind:
        _log_indicators(
            plexos_ind, gtopt_ind, con, box_style, tw, colr, ind_notes or {}
        )


def _log_uc_families(
    plexos_uc: dict[str, Any],
    gtopt_counts: Any,
    con: Any,
    box_style: Any,
    tw: int,
    colr: bool,
) -> None:
    """Render the UC conversion funnel + per-family PLEXOS-vs-gtopt table."""
    from rich.table import Table  # noqa: PLC0415

    check = "[bold green]✓[/bold green]" if colr else "ok"

    # -- Funnel (full drop attribution) --
    funnel = Table(
        box=box_style,
        show_lines=False,
        padding=(0, 1),
        title=(
            "[title]User Constraint Conversion Funnel[/title]"
            if colr
            else "User Constraint Conversion Funnel"
        ),
        title_justify="left",
        width=tw,
    )
    funnel.add_column("Stage", no_wrap=True, min_width=30)
    funnel.add_column("Count", justify="right", min_width=8)
    funnel.add_column("Notes", style="dim")

    raw = plexos_uc["raw_plexos_constraints"]
    lhs = plexos_uc["empty_lhs_dropped"]
    base = plexos_uc["base_emitted"]
    hydro_syn = plexos_uc["hydro_synthesized"]
    plant_syn = plexos_uc["plant_cap_synthesized"]
    # Compare PLEXOS-sourced constraints only — drop the converter-synthesised
    # terminal_value FCF cut from both the detected and emitted totals.
    detected = _comparable_total(plexos_uc["by_family"], plexos_uc["total"])
    active = plexos_uc["active"]
    inactive = plexos_uc["inactive"]
    emitted = _comparable_total(
        gtopt_counts.uc_by_family, gtopt_counts.user_constraints_total
    )
    # Constraints dropped for reasons other than an empty/partial LHS
    # (no System→Constraints membership, no RHS, …).
    other_dropped = max(0, raw - lhs - base)

    funnel.add_row("raw PLEXOS Constraint objects", str(raw), "")
    funnel.add_row(
        "− empty/partial-LHS dropped",
        f"-{lhs}" if lhs else "0",
        "only-unsupported terms (commitment binaries, region aggregates)",
    )
    if other_dropped:
        funnel.add_row(
            "− other dropped",
            f"-{other_dropped}",
            "no System→Constraints membership / no RHS",
        )
    funnel.add_row("= base user constraints", str(base), "")
    if hydro_syn:
        funnel.add_row(
            "+ hydro daily-ramp/discharge", f"+{hydro_syn}", "synthesised by converter"
        )
    if plant_syn:
        funnel.add_row(
            "+ plant-cap config-exclusivity",
            f"+{plant_syn}",
            "synthesised from *_Uniq mutex groups",
        )
    funnel.add_row(
        "= detected (carried on case)",
        str(detected),
        f"{active} active / {inactive} inactive (Include-in-ST=0)",
    )
    emit_note = ""
    if emitted < detected:
        emit_note = "families dropped by --pampl-uc-mode/only/off"
    elif emitted == detected:
        emit_note = "all detected UCs emitted"
    funnel.add_row(
        "= emitted to gtopt JSON",
        str(emitted),
        emit_note,
    )
    # Consistency check: base_emitted + synthesised must equal detected.
    if base + hydro_syn + plant_syn != detected and raw:
        funnel.add_row(
            "[bold yellow]! funnel mismatch[/bold yellow]" if colr else "! mismatch",
            str(detected - (base + hydro_syn + plant_syn)),
            "unaccounted — investigate extraction",
        )
    con.print(funnel)

    # -- Per-family table --
    fam_table = Table(
        box=box_style,
        show_lines=False,
        padding=(0, 1),
        title=(
            "[title]User Constraint Families[/title]"
            if colr
            else "User Constraint Families"
        ),
        title_justify="left",
        width=tw,
    )
    fam_table.add_column("Family", no_wrap=True, min_width=22)
    fam_table.add_column("PLEXOS", justify="right", min_width=7)
    fam_table.add_column("gtopt", justify="right", min_width=7)
    fam_table.add_column("Δ", justify="right", min_width=5)
    fam_table.add_column("Notes", style="dim")

    p_by_fam = plexos_uc["by_family"]
    g_by_fam = gtopt_counts.uc_by_family
    seen: set[str] = set(_COMPARISON_EXCLUDED_FAMILIES)  # skip synthesised
    for fam in UC_FAMILY_ORDER:
        if fam in _COMPARISON_EXCLUDED_FAMILIES:
            continue
        seen.add(fam)
        pv = p_by_fam.get(fam, 0)
        gv = g_by_fam.get(fam, 0)
        if pv == 0 and gv == 0:
            continue
        _add_fam_row(fam_table, fam, pv, gv, check, colr)
    for fam in sorted(set(p_by_fam) | set(g_by_fam)):
        if fam in seen:
            continue
        _add_fam_row(
            fam_table, fam, p_by_fam.get(fam, 0), g_by_fam.get(fam, 0), check, colr
        )
    # Totals row (PLEXOS-sourced only; terminal_value FCF cut excluded).
    _add_fam_row(
        fam_table,
        "TOTAL",
        _comparable_total(p_by_fam, plexos_uc["total"]),
        _comparable_total(g_by_fam, gtopt_counts.user_constraints_total),
        check,
        colr,
        bold=True,
    )
    con.print(fam_table)


def _add_fam_row(
    table: Any,
    fam: str,
    pv: int,
    gv: int,
    check: str,
    colr: bool,
    *,
    bold: bool = False,
) -> None:
    """Add one family row with a coloured Δ and a drop/add note."""
    diff = gv - pv
    if diff == 0:
        delta = check
        note = ""
    elif diff < 0:
        delta = f"[bold yellow]{diff}[/bold yellow]" if colr else str(diff)
        note = "dropped (filtered or not emitted)"
    else:
        delta = f"[bold cyan]+{diff}[/bold cyan]" if colr else f"+{diff}"
        note = "extra on gtopt side"
    label = f"[bold]{fam}[/bold]" if (bold and colr) else fam
    table.add_row(label, str(pv), str(gv), delta, note)


def _log_indicators(
    plexos_ind: dict[str, float],
    gtopt_ind: dict[str, float],
    con: Any,
    box_style: Any,
    tw: int,
    colr: bool,
    notes: dict[str, str] | None = None,
) -> None:
    """Render the global-indicators side-by-side table.

    *notes* maps an indicator label to a free-text note shown in the Notes
    column (e.g. naming a reservoir discarded from the FCF cut).
    """
    from gtopt_check_json._terminal import fmt_num  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415

    notes = notes or {}
    ind_table = Table(
        box=box_style,
        show_lines=False,
        padding=(0, 1),
        title="[title]Global Indicators[/title]" if colr else "Global Indicators",
        title_justify="left",
        width=tw,
    )
    ind_table.add_column("Indicator", no_wrap=True, min_width=26)
    ind_table.add_column("PLEXOS", justify="right", min_width=10)
    ind_table.add_column("gtopt", justify="right", min_width=10)
    ind_table.add_column("Δ%", justify="right", min_width=6)
    ind_table.add_column("Notes", style="dim")

    def _ind_row(label: str, pv: float, gv: float, decimals: int = 2) -> None:
        if pv > 0:
            pct = (gv - pv) / pv * 100.0
            if abs(pct) < 0.5:
                pct_s = "[bold green]✓[/bold green]" if colr else "ok"
            else:
                tag = "bold yellow" if abs(pct) > 5 else "dim"
                raw = f"{pct:+.0f}%"
                pct_s = f"[{tag}]{raw}[/{tag}]" if colr else raw
        elif gv == 0.0:
            pct_s = "[bold green]✓[/bold green]" if colr else "ok"
        else:
            pct_s = ""
        ind_table.add_row(
            label,
            fmt_num(pv, decimals),
            fmt_num(gv, decimals),
            pct_s,
            notes.get(label.strip(), ""),
        )

    _ind_row(
        "gen capacity (MW)",
        plexos_ind.get("total_gen_capacity_mw", 0.0),
        gtopt_ind.get("total_gen_capacity_mw", 0.0),
    )
    _ind_row(
        "  hydro capacity (MW)",
        plexos_ind.get("hydro_capacity_mw", 0.0),
        gtopt_ind.get("hydro_capacity_mw", 0.0),
    )
    _ind_row(
        "  thermal capacity (MW)",
        plexos_ind.get("thermal_capacity_mw", 0.0),
        gtopt_ind.get("thermal_capacity_mw", 0.0),
    )
    _ind_row(
        "line capacity (MW)",
        plexos_ind.get("total_line_capacity_mw", 0.0),
        gtopt_ind.get("total_line_capacity_mw", 0.0),
    )
    _ind_row(
        "up reserve req (MW)",
        plexos_ind.get("up_reserve_req_mw", 0.0),
        gtopt_ind.get("up_reserve_req_mw", 0.0),
    )
    _ind_row(
        "down reserve req (MW)",
        plexos_ind.get("dn_reserve_req_mw", 0.0),
        gtopt_ind.get("dn_reserve_req_mw", 0.0),
    )
    _ind_row(
        "first block demand (MW)",
        plexos_ind.get("first_block_demand_mw", 0.0),
        gtopt_ind.get("first_block_demand_mw", 0.0),
    )
    _ind_row(
        "last block demand (MW)",
        plexos_ind.get("last_block_demand_mw", 0.0),
        gtopt_ind.get("last_block_demand_mw", 0.0),
    )
    _mwh_to_gwh = 1e-3
    _ind_row(
        "total energy (GWh)",
        plexos_ind.get("total_energy_mwh", 0.0) * _mwh_to_gwh,
        gtopt_ind.get("total_energy_mwh", 0.0) * _mwh_to_gwh,
        decimals=2,
    )
    p_dem1 = plexos_ind.get("first_block_demand_mw", 0.0)
    g_dem1 = gtopt_ind.get("first_block_demand_mw", 0.0)
    p_cap = plexos_ind.get("total_gen_capacity_mw", 0.0)
    g_cap = gtopt_ind.get("total_gen_capacity_mw", 0.0)
    _ind_row(
        "capacity adequacy",
        p_cap / p_dem1 if p_dem1 > 0 else 0.0,
        g_cap / g_dem1 if g_dem1 > 0 else 0.0,
        decimals=3,
    )
    _ind_row(
        "first block flow (m³/s)",
        plexos_ind.get("first_block_affluent_avg", 0.0),
        gtopt_ind.get("first_block_affluent_avg", 0.0),
    )
    _ind_row(
        "total water vol (Hm³)",
        plexos_ind.get("total_water_volume_hm3", 0.0),
        gtopt_ind.get("total_water_volume_hm3", 0.0),
    )
    _ind_row(
        "peak demand (MW)",
        plexos_ind.get("peak_demand_mw", 0.0),
        gtopt_ind.get("peak_demand_mw", 0.0),
    )
    _ind_row(
        "demand fail cost ($/MWh)",
        plexos_ind.get("demand_fail_cost", 0.0),
        gtopt_ind.get("demand_fail_cost", 0.0),
    )

    def _both(key: str) -> tuple[float, float]:
        return plexos_ind.get(key, 0.0), gtopt_ind.get(key, 0.0)

    # -- Cost & fuel --
    _ind_row("avg gen cost ($/MWh)", *_both("avg_gcost"))
    _ind_row("min gen cost ($/MWh)", *_both("min_gcost"))
    _ind_row("max gen cost ($/MWh)", *_both("max_gcost"))
    _ind_row("avg heat rate (fuel/MWh)", *_both("avg_heat_rate"), decimals=3)
    _ind_row("avg fuel price ($/unit)", *_both("avg_fuel_price"), decimals=2)
    _ind_row("fuel offtake cap (Σ)", *_both("total_fuel_offtake_cap"), decimals=0)
    _ind_row("fuels with cap (#)", *_both("num_fuel_caps"), decimals=0)
    # -- Commitment --
    p_su, g_su = _both("total_startup_cost")
    _ind_row("total startup cost ($M)", p_su / 1e6, g_su / 1e6)
    p_sd, g_sd = _both("total_shutdown_cost")
    _ind_row("total shutdown cost ($M)", p_sd / 1e6, g_sd / 1e6)
    _ind_row("committable units (#)", *_both("num_committable"), decimals=0)
    # -- Storage & FCF --
    _ind_row("battery energy (MWh)", *_both("total_battery_energy_mwh"), decimals=0)
    _ind_row("battery power (MW)", *_both("total_battery_power_mw"), decimals=0)
    _ind_row("battery round-trip eff", *_both("avg_battery_efficiency"), decimals=3)
    _ind_row("reservoir storage (Σ)", *_both("total_reservoir_storage"), decimals=0)
    _ind_row("reservoir initial vol (Σ)", *_both("total_reservoir_initial"), decimals=0)
    _ind_row("reservoir final vol (Σ)", *_both("total_reservoir_final"), decimals=0)
    p_fcf, g_fcf = _both("fcf_intercept")
    _ind_row("FCF intercept ($M)", p_fcf / 1e6, g_fcf / 1e6, decimals=1)
    _ind_row(
        "water-value reservoirs (#)", *_both("num_water_value_reservoirs"), decimals=0
    )
    # -- Network & hydro --
    _ind_row("lines with loss model (#)", *_both("num_lossy_lines"), decimals=0)
    _ind_row("turbine capacity (MW)", *_both("total_turbine_capacity_mw"), decimals=1)
    _ind_row("up reserve provision (MW)", *_both("total_up_provision_mw"), decimals=0)
    _ind_row("dn reserve provision (MW)", *_both("total_dn_provision_mw"), decimals=0)
    _ind_row("up reserve margin (MW)", *_both("up_reserve_margin_mw"), decimals=0)
    _ind_row("dn reserve margin (MW)", *_both("dn_reserve_margin_mw"), decimals=0)
    _ind_row("gen pmax profiles (#)", *_both("num_gen_pmax_profiles"), decimals=0)
    _ind_row("provision profiles (#)", *_both("num_provision_profiles"), decimals=0)
    _ind_row(
        "fuels w/ emission factor (#)", *_both("num_fuel_emission_factors"), decimals=0
    )
    _ind_row("fixed-load energy (GWh)", *_both("fixed_load_energy_gwh"), decimals=1)
    _ind_row("renewable energy (GWh)", *_both("renewable_energy_gwh"), decimals=1)

    # -- input-file read receipts -------------------------------------------
    # One aggregate per PLEXOS input file no other indicator reflects, so a
    # file silently not read shows up as a zero (see input-file table below).
    _ind_row(
        "gen fixed-load floor (Σ MW)", *_both("total_gen_min_stable_mw"), decimals=0
    )
    _ind_row(
        "commitment min-stable (Σ MW)", *_both("commitment_min_stable_mw"), decimals=0
    )
    _ind_row(
        "initial gen power (Σ MW)", *_both("total_initial_gen_power_mw"), decimals=0
    )
    _ind_row("units initially on (#)", *_both("num_units_initial_on"), decimals=0)
    _ind_row(
        "initial commit hours (Σ)", *_both("total_initial_commit_hours"), decimals=0
    )
    _ind_row("units w/ ramp limit (#)", *_both("num_units_with_ramp_limit"), decimals=0)
    _ind_row(
        "battery initial energy (MWh)", *_both("total_battery_initial_mwh"), decimals=0
    )
    _ind_row("reservoir min vol (Σ)", *_both("total_reservoir_min_vol"), decimals=0)

    con.print(ind_table)


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------


def log_conversion_stats(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> None:
    """Print a plp2gtopt-style "Conversion Results" base-stats table.

    Mirrors :func:`plp2gtopt.plp2gtopt._log_stats`: a single Property/Value
    table of the gtopt-side element counts (units, lines, demand, …) plus the
    PLEXOS extras and the user-constraint family breakdown, all sourced from
    :func:`gtopt_check_json._info.compute_counts` (the JSON authority) and
    rendered with the shared rich terminal helpers (the nice TUI).
    """
    from gtopt_check_json._terminal import print_table  # noqa: PLC0415

    c = _gtopt_element_counts(planning, base_dir=base_dir)
    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})
    mo = opts.get("model_options", opts) if isinstance(opts, dict) else {}

    rows: list[tuple[str, str]] = [
        ("System name", str(sys_data.get("name", "(unnamed)"))),
    ]
    if sys_data.get("version"):
        rows.append(("System version", str(sys_data.get("version"))))

    # -- Element counts (skip zero-count for cleaner output) --
    elems: list[tuple[str, int]] = [
        ("Buses", c.buses),
        ("Generators", c.generators),
    ]
    rows.extend((k, str(v)) for k, v in elems if v > 0)
    # Generator breakdown by type (indented).
    for gtype in sorted(c.gen_by_type):
        rows.append((f"  {gtype}", str(c.gen_by_type[gtype])))
    elems2: list[tuple[str, int]] = [
        ("Generator profiles", c.generator_profiles),
        ("Demands", c.demands),
        ("Lines", c.lines),
        ("Batteries", c.batteries),
        ("Converters", c.converters),
        ("Reserve zones", c.reserve_zones),
        ("Reserve provisions", c.reserve_provisions),
        ("Junctions", c.junctions),
        ("Waterways", c.waterways),
        ("Flows", c.flows),
        ("Reservoirs", c.reservoirs),
        ("Turbines", c.turbines),
        ("Fuels", c.fuels),
        ("Commitments", c.commitments),
        ("Flow rights", c.flow_rights),
        ("Decision variables", c.decision_variables),
    ]
    rows.extend((k, str(v)) for k, v in elems2 if v > 0)

    # -- User constraints (total + per-family + active/inactive) --
    if c.user_constraints_total > 0:
        rows.append(("User constraints", str(c.user_constraints_total)))
        rows.append(
            (
                "  active / inactive",
                f"{c.user_constraints_active} / {c.user_constraints_inactive}",
            )
        )
        for fam in UC_FAMILY_ORDER:
            n = c.uc_by_family.get(fam, 0)
            if n > 0:
                rows.append((f"  {fam}", str(n)))
        uc_files = sys_data.get("user_constraint_files") or []
        if uc_files:
            rows.append(("Constraint files", str(len(uc_files))))

    # -- Boundary cuts --
    if c.boundary_cuts:
        rows.append(
            (
                "Boundary cuts",
                f"{c.boundary_cuts} ({c.boundary_state_variables} state variables)",
            )
        )

    # -- Simulation --
    rows.append(("Blocks", str(c.num_blocks)))
    rows.append(("Stages", str(c.num_stages)))
    rows.append(("Scenarios", str(c.num_scenarios)))
    total_hours = sum(b.get("duration", 0.0) or 0.0 for b in sim.get("block_array", []))
    if total_hours > 0:
        rows.append(("Total period", f"{total_hours:,.0f} h"))

    # -- Demand-fail cost --
    demands = sys_data.get("demand_array", [])
    fcosts = [d["fcost"] for d in demands if isinstance(d.get("fcost"), (int, float))]
    if fcosts:
        lo, hi = min(fcosts), max(fcosts)
        cost = f"{lo:.2f}" if lo == hi else f"{lo:.2f}–{hi:.2f}"
        rows.append(("Demand fail cost", f"{cost} $/MWh"))

    # -- Key options --
    rows.append(("use_kirchhoff", str(mo.get("use_kirchhoff", False))))
    rows.append(("use_single_bus", str(mo.get("use_single_bus", False))))
    rows.append(("scale_objective", str(mo.get("scale_objective", 1000))))

    print_table(
        headers=["Property", "Value"],
        rows=rows,
        aligns=["left", "right"],
        title="Conversion Results",
    )


def _log_drop_funnel(case: PlexosCase, gtopt_counts: Any) -> None:
    """Print the per-class conversion drop funnel (raw PLEXOS → gtopt emitted).

    Answers "did we drop something" by showing, for each physical class, the
    raw PLEXOS object count vs what was emitted, with the drop reason.  Only
    rendered when raw counts were captured (``case.raw_class_counts``).
    """
    raw = case.raw_class_counts
    if not raw:
        return
    from gtopt_check_json._terminal import console, print_section, table_width  # noqa: PLC0415
    from rich.box import ASCII, ROUNDED  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415

    con = console()
    colr = con.is_terminal
    g = gtopt_counts

    # (label, raw PLEXOS count, gtopt emitted, drop-note, gain-note) — only
    # classes that change.  gtopt can emit MORE than the raw XML count when the
    # converter recovers CSV-only objects (e.g. CSV-only thermals folded back in
    # as zero-capacity audit entries), so each class carries a separate note for
    # the gained direction.
    storage_note = "pondage/tailrace demoted to junction; _GNL_INF dropped"
    gen_gain = "recovered CSV-only thermals as zero-capacity audit entries"
    candidates = [
        (
            "Lines",
            raw.get("Line", 0),
            g.lines,
            "mothballed / all-zero / no endpoint",
            "",
        ),
        ("Batteries", raw.get("Battery", 0), g.batteries, "_AUX reserve buffers", ""),
        ("Storage → Reservoirs", raw.get("Storage", 0), g.reservoirs, storage_note, ""),
        ("Generators", raw.get("Generator", 0), g.generators, "", gen_gain),
    ]
    rows = [row for row in candidates if row[1]]
    if not rows:
        return

    print_section("Conversion Drop Funnel")
    table = Table(
        box=ROUNDED if colr else ASCII,
        show_lines=False,
        padding=(0, 1),
        width=table_width(),
    )
    table.add_column("Class", no_wrap=True, min_width=22)
    table.add_column("PLEXOS raw", justify="right", min_width=10)
    table.add_column("gtopt", justify="right", min_width=8)
    table.add_column("Δ", justify="right", min_width=8)
    table.add_column("Reason", style="dim")
    for lbl, rawn, emit, drop_note, gain_note in rows:
        dropped = rawn - emit
        if dropped == 0:
            dtxt = "[bold green]✓[/bold green]" if colr else "ok"
            note = ""
        elif dropped > 0:
            dtxt = f"[bold yellow]-{dropped}[/bold yellow]" if colr else f"-{dropped}"
            note = drop_note
        else:
            gained = -dropped
            dtxt = f"[bold cyan]+{gained}[/bold cyan]" if colr else f"+{gained}"
            note = gain_note
        table.add_row(lbl, str(rawn), str(emit), dtxt, note)
    con.print(table)


def _log_input_files() -> None:
    """Print the PLEXOS input-file ledger: every file the converter reads,
    what it carries, and the report indicator that proves it was read.

    This is the "did we read it" answer the indicators provide: each file maps
    to an indicator (element count or Global Indicator) that is non-zero only
    when the file was parsed, so a zero there flags a missing / skipped input.
    """
    from gtopt_check_json._terminal import console, print_section, table_width  # noqa: PLC0415
    from rich.box import ASCII, ROUNDED  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415

    con = console()
    colr = con.is_terminal

    print_section("PLEXOS Input Files → Indicators")
    table = Table(
        box=ROUNDED if colr else ASCII,
        show_lines=False,
        padding=(0, 1),
        width=table_width(),
    )
    table.add_column("Input file", no_wrap=True, min_width=28)
    table.add_column("Carries", min_width=24)
    table.add_column("Linked indicator", style="dim")
    for fname, carries, indicator in _INPUT_FILE_INDICATORS:
        table.add_row(fname, carries, indicator)
    con.print(table)
    con.print(
        "[dim]A zero / blank in a linked indicator means that file was not "
        "read (missing or empty in the bundle).[/dim]"
        if colr
        else "Note: a zero/blank linked indicator means the file was not read."
    )


def run_post_check(
    planning: dict[str, Any],
    case: PlexosCase,
    output_dir: str | Path | None = None,
) -> int:
    """Print the PLEXOS↔gtopt comparison and run structural validation.

    Mirrors :func:`plp2gtopt.plp2gtopt.run_post_check`: build the element /
    UC / indicator comparison from the parsed *case* and the generated
    *planning* (gtopt side via gtopt_check_json), then run
    ``run_all_checks`` on the planning dict.

    Returns the number of CRITICAL findings (``0`` means success).
    """
    base_dir = str(output_dir) if output_dir is not None else ""

    # plp2gtopt-style base-stats table first (units / lines / demand / … +
    # the new PLEXOS extras and UC families), then the side-by-side comparison.
    log_conversion_stats(planning, base_dir=base_dir)

    plexos_counts = _plexos_element_counts(case)
    gtopt_counts = _gtopt_element_counts(planning, base_dir=base_dir)
    plexos_uc = _plexos_uc_classification(case)
    plexos_ind, gtopt_ind = compute_comparison_indicators(
        case, planning, base_dir=base_dir
    )
    # Note any PLEXOS water-value reservoirs dropped from the FCF cut because
    # they are not bundle reservoirs (so the count compares like-for-like).
    ind_notes: dict[str, str] = {}
    if case.boundary_cut is not None:
        res_names = {r.name for r in case.reservoirs}
        discarded = sorted(r for r in case.boundary_cut.slopes if r not in res_names)
        if discarded:
            ind_notes["water-value reservoirs (#)"] = (
                f"discarded (not a bundle reservoir): {', '.join(discarded)}"
            )
    _log_comparison(
        plexos_counts,
        gtopt_counts,
        plexos_uc,
        plexos_ind,
        gtopt_ind,
        ind_notes,
    )
    _log_drop_funnel(case, gtopt_counts)
    _log_input_files()

    # -- Structural validation (optional) --
    try:
        from gtopt_check_json._checks import (  # noqa: PLC0415
            Severity,
            run_all_checks,
        )
        from gtopt_check_json._terminal import (  # noqa: PLC0415
            print_status,
            print_summary,
            print_table,
        )
    except ImportError:
        logger.debug("gtopt_check_json not available; skipping JSON validation")
        return 0

    findings = run_all_checks(
        planning, enabled_checks=None, ai_options=None, base_dir=base_dir
    )
    if not findings:
        print_status("All checks passed — no issues found.", ok=True)
        return 0

    rows: list[tuple[str, str, str]] = []
    critical_count = warning_count = note_count = 0
    for finding in findings:
        if finding.severity == Severity.CRITICAL:
            critical_count += 1
        elif finding.severity == Severity.WARNING:
            warning_count += 1
        else:
            note_count += 1
        rows.append((finding.severity.name, finding.message, finding.action))

    print_table(
        headers=["Severity", "Description", "Action / Notes"],
        rows=rows,
        aligns=["left", "left", "left"],
        title="Warnings",
        styles=["warn", "", "dim"],
        min_widths=[8, None, None],
        wraps=[False, True, True],
        show_lines=len(rows) > 1,
    )
    print_summary(critical_count, warning_count, note_count)
    return critical_count


def compare_plexos_bundle(options: dict[str, Any]) -> int:
    """Standalone re-comparison: parse a PLEXOS bundle vs an existing JSON.

    Recognised option keys: ``input_bundle`` (PLEXOS bundle path) and
    ``compare_json`` (path to a previously-converted gtopt planning JSON).
    The gtopt side is read from disk and counted by gtopt_check_json; the
    PLEXOS side is freshly parsed.  Returns the number of CRITICAL findings.
    """
    import dataclasses  # noqa: PLC0415
    import json  # noqa: PLC0415

    from .parsers import extract_case  # noqa: PLC0415
    from .plexos_loader import locate_bundle  # noqa: PLC0415

    raw_input = options.get("input_bundle")
    json_path = options.get("compare_json")
    if raw_input is None or json_path is None:
        raise ValueError(
            "compare_plexos_bundle requires 'input_bundle' and 'compare_json'"
        )
    json_file = Path(json_path)
    with json_file.open("r", encoding="utf-8") as fh:
        planning = json.load(fh)
    with locate_bundle(Path(raw_input)) as bundle:
        case = extract_case(bundle)
    # The conversion path attaches the PLEXOS block layout (from the .accdb) to
    # the case so energy/water tile a daily profile across the horizon.  The
    # standalone path has no .accdb, so reconstruct an equivalent layout from
    # the JSON's block durations (only its block count and total hours matter
    # to ``_durations_for``) — otherwise a 24h daily profile would integrate to
    # a single day's energy.
    layout = _layout_from_planning(planning)
    if layout:
        case = dataclasses.replace(
            case, bundle=dataclasses.replace(case.bundle, block_layout=layout)
        )
    return run_post_check(planning, case, output_dir=json_file.parent)


def _layout_from_planning(
    planning: dict[str, Any],
) -> tuple[tuple[int, ...], ...]:
    """Reconstruct a block layout from a planning dict's ``block_array``.

    Only the block count and per-block interval (hour) counts matter to
    :func:`_durations_for`, so each block maps to a placeholder tuple of
    ``round(duration)`` interval ids.
    """
    blocks = planning.get("simulation", {}).get("block_array", [])
    layout: list[tuple[int, ...]] = []
    for blk in blocks:
        n_iv = max(1, int(round(float(blk.get("duration", 1.0) or 1.0))))
        layout.append(tuple(range(n_iv)))
    return tuple(layout)


__all__ = [
    "compare_plexos_bundle",
    "compute_comparison_indicators",
    "log_conversion_stats",
    "run_post_check",
]
