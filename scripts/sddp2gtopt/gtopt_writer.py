"""Build a gtopt JSON planning from parsed SDDP specs.

The output schema matches the small reference cases under
``cases/c0`` and ``cases/s1b`` — three top-level keys ``options``,
``simulation`` and ``system`` — so it can be solved by ``gtopt
--lp-only`` directly without further post-processing.

v0 produced a single-bus, monolithic LP.  The post-2026-05-16 update
adds **multi-system** support: each ``PSRSystem`` becomes its own
gtopt ``Bus`` and every ``PSRThermalPlant`` / ``PSRHydroPlant`` /
``PSRDemand`` is routed to the bus matching its ``system`` cross-ref
(``system_ref`` on the parsed specs).  Inter-system topology
(``PSRInterconnection`` → gtopt ``Line``) remains a P4 item; without
interconnections the converted multi-system LP is a set of disjoint
single-bus problems, which is the correct semantics for SDDP cases
where the user has deliberately not declared any inter-area links.
"""

from __future__ import annotations

import logging
from typing import Any

from gtopt_shared.json_planning import (  # noqa: F401  pylint: disable=unused-import
    build_json_item,
    write_planning,  # re-exported for back-compat
)

from .entities import (
    DemandSpec,
    HydroSpec,
    StudySpec,
    SystemSpec,
    ThermalSpec,
)


logger = logging.getLogger(__name__)


# Hours per stage by PSR Tipo_Etapa code (1=weekly, 2=monthly, 3=trimester).
# These are nominal averages — real cases vary slightly with calendar
# month, but the LP only needs a self-consistent unit so this is fine
# for v0.
_STAGE_HOURS: dict[int, float] = {1: 168.0, 2: 730.0, 3: 2190.0}


def _stage_hours(stage_type: int) -> float:
    return _STAGE_HOURS.get(stage_type, 730.0)


def build_options(
    study: StudySpec,
    *,
    use_single_bus: bool = True,
    use_kirchhoff: bool = False,
    hydro_topology: bool = False,
) -> dict[str, Any]:
    """Map :class:`StudySpec` onto gtopt's top-level ``options`` block.

    The 11 legacy top-level mirror keys (``use_single_bus`` /
    ``use_kirchhoff`` / ``demand_fail_cost`` / ``scale_objective`` / …)
    were moved into ``options.model_options.*`` by the 2026-05-17
    StrictParsePolicy migration; emitting them at top level now causes
    a JSON parse error.  Keep ``annual_discount_rate`` / ``output_*``
    at top level (those stayed) and nest the rest under
    ``model_options``.

    When ``hydro_topology`` is set the case carries a boundary cut on the
    reservoir end-volumes: the cut rows are divided by ``scale_objective``
    on assembly, so we drop the objective scale to ``1.0`` (raw money) and
    let ``auto_scale`` equilibrate, exactly as the reference SDDP cases do.
    """
    model_options: dict[str, Any] = {
        "use_single_bus": use_single_bus,
        "use_kirchhoff": use_kirchhoff,
        "demand_fail_cost": float(study.deficit_cost),
        "scale_objective": 1.0 if hydro_topology else 1000,
        # PSR networks carry near-zero-reactance bus-tie / coupler circuits
        # (``reactance_pu`` ~5e-4, just above the ``_MIN_REACTANCE`` 1e-5 floor).
        # Under ``use_kirchhoff`` those near-short-circuits attract
        # disproportionate DC flow (f = Δθ / X) and saturate their ``tmax``,
        # pinning bus angles so parallel higher-reactance ties — with ample
        # spare capacity — cannot feed load pockets.  The result is spurious
        # congestion + demand curtailment (a 58-bus, 335 MW GUA pocket priced
        # at the 315 $/MWh fail cost).  Promote any circuit with
        # ``|x_pu| < threshold`` to a KVL-free "DC line" (flow-bounded only) so
        # power distributes by economics.  1e-3 removes the curtailment
        # (9160 → 28 MWh on the GUA weekly case) and saturates (identical up to
        # 1e-2); it sits safely between the coupler scale (~5e-4) and real
        # transmission reactances (≳1e-2).
        "dc_line_reactance_threshold": 1.0e-3,
    }
    if hydro_topology:
        model_options["auto_scale"] = True
    return {
        "annual_discount_rate": study.discount_rate,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "model_options": model_options,
    }


def build_simulation(study: StudySpec) -> dict[str, Any]:
    """Map :class:`StudySpec` onto ``simulation`` (stages, blocks, scenarios).

    Each stage gets ``num_blocks`` sequential blocks, each of duration
    ``study.block_hours`` (or ``_stage_hours(stage_type) / num_blocks``).

    For a **multi-stage** horizon the SDDP/cascade decomposition layer is
    emitted too: one ``phase`` per stage (SDDP phase = PSR stage), a single
    ``scene`` over the lone deterministic scenario, and a single
    ``aperture`` referencing it.  These are what let gtopt run the case
    under the SDDP / cascade methods (which need ≥ 2 phases and drive the
    cross-phase reservoir state links that carry the water value); a
    single-stage horizon omits them and solves monolithically.
    """
    block_hours = study.block_hours or (
        _stage_hours(study.stage_type) / max(study.num_blocks, 1)
    )
    block_array: list[dict[str, Any]] = []
    stage_array: list[dict[str, Any]] = []
    bid = 1
    for s in range(study.num_stages):
        first_block = bid - 1
        for _ in range(study.num_blocks):
            block_array.append({"uid": bid, "duration": block_hours})
            bid += 1
        stage_array.append(
            {
                "uid": s + 1,
                "first_block": first_block,
                "count_block": study.num_blocks,
                "active": 1,
            }
        )
    sim: dict[str, Any] = {
        "block_array": block_array,
        "stage_array": stage_array,
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    }
    # The SDDP decomposition layer is emitted only for the staged ``.dat``
    # NCP path (marked by an explicit hourly ``block_hours``); the generic
    # json front-end keeps its monolithic multi-stage layout unchanged.
    if study.num_stages > 1 and study.block_hours > 0:
        sim["phase_array"] = [
            {"uid": s + 1, "first_stage": s, "count_stage": 1, "apertures": [1]}
            for s in range(study.num_stages)
        ]
        sim["scene_array"] = [{"uid": 1, "first_scenario": 0, "count_scenario": 1}]
        sim["aperture_array"] = [
            {"uid": 1, "source_scenario": 1, "probability_factor": 1.0}
        ]
    return sim


def _bus_name(system: SystemSpec) -> str:
    """Synthesised single-bus name for a system (avoids whitespace)."""
    return f"sys_{system.code}_bus"


def _build_system_bus_map(
    systems: list[SystemSpec],
) -> tuple[dict[int, str], str]:
    """Index ``PSRSystem.reference_id`` → gtopt bus name.

    Returns ``(bus_by_ref, fallback_bus)``.  Specs that lack a
    ``system_ref`` (some legacy cases) fall back to the first system's
    bus — the same behaviour as the pre-multi-system v0 default.
    """
    bus_by_ref: dict[int, str] = {s.reference_id: _bus_name(s) for s in systems}
    fallback_bus = _bus_name(systems[0])
    return bus_by_ref, fallback_bus


def _resolve_bus(
    spec_system_ref: int | None,
    bus_by_ref: dict[int, str],
    fallback_bus: str,
) -> str:
    """Map a spec's ``system_ref`` to its gtopt bus name.

    Logs a debug message and routes to ``fallback_bus`` when the ref
    is missing or unresolvable — guards against partial / hand-crafted
    fixtures whose entities omit the ``system`` cross-ref.
    """
    if spec_system_ref is None:
        return fallback_bus
    bus = bus_by_ref.get(spec_system_ref)
    if bus is None:
        logger.debug(
            "system_ref %d not in PSRSystem index; routing to fallback bus %s",
            spec_system_ref,
            fallback_bus,
        )
        return fallback_bus
    return bus


def _gen_gcost(plant: ThermalSpec) -> float:
    """Pick a representative gcost for a thermal plant.

    PSR encodes up to three segments; gtopt's simple ``generator``
    schema expects a scalar. v0 collapses to the cheapest non-trivial
    segment so dispatch order matches the PSR merit order. Multi-segment
    bid curves are a P2 enhancement.
    """
    if plant.g_segments:
        return min(cost for _, cost in plant.g_segments)
    return 0.0


def build_thermal_generators(
    plants: list[ThermalSpec],
    *,
    bus_name: str | None = None,
    bus_by_ref: dict[int, str] | None = None,
    fallback_bus: str | None = None,
    start_uid: int = 1,
) -> list[dict[str, Any]]:
    """Map :class:`ThermalSpec` list onto gtopt ``generator_array`` entries.

    Two calling conventions are supported:

    * **Single-bus (legacy):** pass ``bus_name`` and every plant maps
      onto that single bus.  Preserved so older call sites keep
      working unchanged.
    * **Multi-system:** pass ``bus_by_ref`` and ``fallback_bus``; each
      plant routes to ``bus_by_ref[plant.system_ref]``.
    """
    if bus_by_ref is None:
        if bus_name is None:
            raise ValueError("build_thermal_generators: pass bus_name or bus_by_ref")
        bus_by_ref = {}
        fallback_bus = bus_name
    elif fallback_bus is None:
        raise ValueError("build_thermal_generators: bus_by_ref needs fallback_bus")

    out: list[dict[str, Any]] = []
    uid = start_uid
    for plant in plants:
        bus = _resolve_bus(plant.system_ref, bus_by_ref, fallback_bus)
        out.append(
            {
                "uid": uid,
                "name": plant.name or f"thermal_{plant.code}",
                "bus": bus,
                "pmin": plant.pmin,
                "pmax": plant.pmax,
                "gcost": _gen_gcost(plant),
                "capacity": plant.pmax,
            }
        )
        uid += 1
    return out


def build_hydro_generators(
    plants: list[HydroSpec],
    *,
    bus_name: str | None = None,
    bus_by_ref: dict[int, str] | None = None,
    fallback_bus: str | None = None,
    start_uid: int,
    gcost: float = 0.0,
) -> list[dict[str, Any]]:
    """Hydros are flattened to zero-cost generators in v0.

    Reservoir + turbine + inflow modelling is the P2 deliverable
    (see DESIGN.md). Until then we treat each hydro as a free-fuel
    generator capped at ``PotInst`` so the LP at least contains the
    right capacity.

    Bus-routing follows the same dual-convention as
    :func:`build_thermal_generators` — single-bus (``bus_name``) or
    per-system (``bus_by_ref`` + ``fallback_bus``).
    """
    if bus_by_ref is None:
        if bus_name is None:
            raise ValueError("build_hydro_generators: pass bus_name or bus_by_ref")
        bus_by_ref = {}
        fallback_bus = bus_name
    elif fallback_bus is None:
        raise ValueError("build_hydro_generators: bus_by_ref needs fallback_bus")

    out: list[dict[str, Any]] = []
    uid = start_uid
    for plant in plants:
        bus = _resolve_bus(plant.system_ref, bus_by_ref, fallback_bus)
        out.append(
            {
                "uid": uid,
                "name": plant.name or f"hydro_{plant.code}",
                "bus": bus,
                "pmin": 0.0,
                "pmax": plant.p_inst,
                "gcost": gcost,
                "capacity": plant.p_inst,
            }
        )
        uid += 1
    return out


def _normalize_demand_profile(
    demand: DemandSpec, study: StudySpec
) -> list[list[float]]:
    """Build the ``lmax[stage][block]`` matrix.

    Two sources:

    * ``block_values`` (PSR ``.dat`` NCP path) — an explicit per-block MW
      series for a single stage, emitted verbatim as ``lmax[0]``.
    * ``profile`` (json path) — PSR ``Demanda(1)`` in **GWh per stage**;
      gtopt's ``lmax`` is **MW per block**, so we divide by block hours
      and replicate across blocks.
    """
    if demand.block_values:
        bv = list(demand.block_values)
        nb, ns = study.num_blocks, study.num_stages
        if ns > 1 and len(bv) == ns * nb:
            # Multi-stage NCP: chunk the flat hourly horizon into stages.
            return [bv[s * nb : (s + 1) * nb] for s in range(ns)]
        return [bv]

    block_hours = study.block_hours or (
        _stage_hours(study.stage_type) / max(study.num_blocks, 1)
    )
    profile = list(demand.profile)
    if len(profile) < study.num_stages:
        profile += [0.0] * (study.num_stages - len(profile))
    rows: list[list[float]] = []
    for s in range(study.num_stages):
        gwh = profile[s]
        mw = (gwh * 1000.0) / block_hours if block_hours > 0 else 0.0
        rows.append([mw] * study.num_blocks)
    return rows


def build_demands(
    demands: list[DemandSpec],
    study: StudySpec,
    *,
    bus_name: str | None = None,
    bus_by_ref: dict[int, str] | None = None,
    fallback_bus: str | None = None,
    start_uid: int = 1,
) -> list[dict[str, Any]]:
    """Map demands onto gtopt ``demand_array`` with inline ``lmax`` matrices.

    Same dual bus-routing convention as the generator builders.
    """
    if bus_by_ref is None:
        if bus_name is None:
            raise ValueError("build_demands: pass bus_name or bus_by_ref")
        bus_by_ref = {}
        fallback_bus = bus_name
    elif fallback_bus is None:
        raise ValueError("build_demands: bus_by_ref needs fallback_bus")

    out: list[dict[str, Any]] = []
    uid = start_uid
    for d in demands:
        bus = _resolve_bus(d.system_ref, bus_by_ref, fallback_bus)
        out.append(
            {
                "uid": uid,
                "name": d.name or f"demand_{d.code}",
                "bus": bus,
                "lmax": _normalize_demand_profile(d, study),
            }
        )
        uid += 1
    return out


# Minimum |reactance| (p.u.) for a circuit, so the DC flow equation
# f = (θ_a − θ_b) / X stays well-posed on bus-tie / zero-impedance links.
_MIN_REACTANCE = 1.0e-5
# Large finite tmax for circuits with no declared rating.
_UNLIMITED_RATING = 99999.0


def _net_bus_ref(number: int) -> str:
    """gtopt bus reference name for a PSR network bus number."""
    return f"b{number}"


# Large finite drain capacity (m³/s) so every junction can shed surplus
# water (reservoir overflow / run-of-river excess) — PSR's default
# ``SerVer = 0`` spill-to-sea behaviour, which keeps the cascade feasible.
_SPILL_CAP = 1.0e6

# Reservoir flow→volume conversion for PSR's units (hm³ storage, m³/s flow):
# 1 m³/s · 1 h = 3600 m³ = 0.0036 hm³.  Set explicitly on every reservoir —
# gtopt's LP defaults an *absent* flow_conversion_rate to 3.6 (the dam³
# convention, 1000× larger), which spuriously overflows hm³ reservoirs.
_FLOW_CONVERSION_RATE = 0.0036


def _hy_ref(name: str) -> str:
    """A reference-safe token for a hydro element label (no whitespace)."""
    return name.strip().replace(" ", "_")


def _pmax_cap(profile: list[float], study: StudySpec) -> Any:
    """Generator ``pmax`` from a PSR ``cprmx*`` per-hour cap profile.

    Returns a scalar when the cap is constant, a ``[stage][block]`` schedule
    when it varies over the matching horizon, the mean as a scalar on a
    length mismatch, or ``None`` when there is no cap (caller keeps installed).
    """
    if not profile:
        return None
    if all(abs(v - profile[0]) < 1e-9 for v in profile):
        return profile[0]
    nb, ns = study.num_blocks, study.num_stages
    if ns > 1 and len(profile) == ns * nb:
        return [profile[s * nb : (s + 1) * nb] for s in range(ns)]
    if len(profile) == nb:
        return [list(profile)]
    return sum(profile) / len(profile)


def _amm_override(tplant: Any, pmax_default: Any, study: StudySpec) -> tuple[Any, Any]:
    """Apply a RESTMEX (AMM) constraint to a generator's ``(pmin, pmax)``.

    ``<`` caps generation (pmax), ``>`` floors it (pmin), ``=`` fixes it; hours
    with no RHS (``None``) fall back to the default bound (0 / installed-or-cap).
    Returns ``(0.0, pmax_default)`` when the unit has no AMM constraint.
    """
    tp, prof = tplant.amm_tipo, tplant.amm_profile
    if tp not in ("<", ">", "=") or not prof:
        return 0.0, pmax_default
    total = max(1, study.num_stages * study.num_blocks)
    def_pmax = pmax_default if isinstance(pmax_default, (int, float)) else tplant.pmax
    amm = [prof[i] if i < len(prof) else None for i in range(total)]
    if tp == "<":
        return 0.0, _pmax_cap([v if v is not None else def_pmax for v in amm], study)
    if tp == ">":
        return _pmax_cap(
            [v if v is not None else 0.0 for v in amm], study
        ), pmax_default
    return (
        _pmax_cap([v if v is not None else 0.0 for v in amm], study),
        _pmax_cap([v if v is not None else def_pmax for v in amm], study),
    )


def _build_boundary_cut_csv(cuts: list[tuple[str, float, float]]) -> str:
    """Assemble the single-row boundary-cut CSV for the reservoir water values.

    ``cuts`` is ``[(reservoir_name, water_value [$/hm³], efin [hm³]), …]``.
    The cut is the linear future-cost function tangent at each reservoir's
    **expected end-of-horizon volume** ``efin`` (PSR ``volfincp``, else
    ``eini``) — not at the full ``emax``.  gtopt installs a SINGLE cut as the
    equality ``α + Σ wvᵣ·efinᵣ = rhs`` with α freed (``single_cut_equality``),
    so ``α = rhs − Σ wvᵣ·Vᵣ``.  Choosing ``rhs = Σ wvᵣ·efinᵣ`` centres α at 0
    when each reservoir ends at its expected volume (α < 0 when more water is
    conserved, α > 0 when drawn down) and prices every reservoir's terminal
    storage at ``wvᵣ`` [$/hm³] on BOTH sides of the target — the continuous
    PLEXOS-style water value, evaluated at the operating point.
    """
    names = [c[0] for c in cuts]
    rhs = sum(wv * eini for _, wv, eini in cuts)
    coeffs = [-wv for _, wv, _ in cuts]
    header = "iteration,scene,rhs," + ",".join(names)
    row = "1,1," + repr(rhs) + "," + ",".join(repr(c) for c in coeffs)
    return header + "\n" + row + "\n"


def _inflow_sched(daily: list[float], study: StudySpec) -> Any:
    """Map a per-day inflow series to a ``[stage][block]`` discharge schedule.

    The hourly model spreads each day's inflow across its 24 blocks; a staged
    model (one stage per day) takes one value per stage.  Collapses to a scalar
    only when the whole dispatch horizon is a single constant inflow.
    """
    if not daily:
        return None
    nb, ns = study.num_blocks, study.num_stages
    if ns > 1:
        stage_block = [[float(daily[min(s, len(daily) - 1)])] * nb for s in range(ns)]
    else:
        per_day = 24 if nb % 24 == 0 else max(1, nb // max(len(daily), 1))
        stage_block = [
            [float(daily[min(b // per_day, len(daily) - 1)]) for b in range(nb)]
        ]
    flat = [v for row in stage_block for v in row]
    if all(abs(v - flat[0]) < 1e-12 for v in flat):
        return flat[0]  # constant horizon → scalar (gtopt broadcasts)
    # Flow.discharge is OptSTBRealFieldSched → [scene][stage][block]; one scene.
    return [stage_block]


def _build_water_network(
    hydros: list[Any],
    junctions_by_code: dict[int, str],
    study: StudySpec,
) -> tuple[
    list[dict[str, Any]],
    list[dict[str, Any]],
    list[dict[str, Any]],
    list[dict[str, Any]],
    str | None,
]:
    """Build the hydraulic elements + boundary cut for the dispatched hydros.

    For every hydro that is emitted as a generator we create:

    * a **Junction** (its headwater node), ``drain`` so surplus water spills
      to sea (PSR ``SerVer = 0``);
    * a **Reservoir** on that junction when the plant has storage
      (``VMax > 0``) — ``emin``/``emax`` = ``VMin``/``VMax`` [hm³], initial
      volume from the cota–vol interpolation, ``use_state_variable`` so the
      end-volume couples to the boundary cut;
    * a **Turbine** debiting the plant's junction and crediting the
      downstream plant's junction (``VAA``); terminal plants drain to sea;
    * a **Flow** injecting the natural inflow [m³/s] into the junction.

    Returns ``(junction_array, reservoir_array, turbine_array, flow_array,
    boundary_cut_csv)`` — the last is ``None`` when no reservoir carries a
    positive water value.
    """
    junction_array: list[dict[str, Any]] = []
    reservoir_array: list[dict[str, Any]] = []
    turbine_array: list[dict[str, Any]] = []
    flow_array: list[dict[str, Any]] = []
    cuts: list[tuple[str, float, float]] = []

    for juid, h in enumerate(hydros, start=1):
        junction_array.append(
            {
                "uid": juid,
                "name": junctions_by_code[h.code],
                "drain": True,
                "drain_capacity": _SPILL_CAP,
            }
        )

    ruid = tuid = fuid = 1
    for h in hydros:
        jname = junctions_by_code[h.code]
        if h.vmax > 0.0:
            res_name = f"rs_{_hy_ref(h.name)}"
            reservoir = {
                "uid": ruid,
                "name": res_name,
                "junction": jname,
                "capacity": h.vmax,
                "emin": h.vmin,
                "emax": h.vmax,
                "eini": h.eini,
                # MUST be set: PSR uses hm³ volumes + m³/s flows, so the
                # conversion is 0.0036 hm³/(m³/s·h).  gtopt's LP defaults an
                # ABSENT flow_conversion_rate to 3.6 (the dam³ convention) —
                # 1000× too large — which fills the reservoirs in minutes
                # (spurious overflows).  Pin it to the hm³ value.
                "flow_conversion_rate": _FLOW_CONVERSION_RATE,
                "use_state_variable": True,
            }
            if h.water_value > 0.0:
                if h.efin > 0.0:
                    # A real end-volume is shipped (PSR ``volfincp``): emit a soft
                    # end target ``vol_end + slack >= efin`` priced at the water
                    # value, and linearise the cut at the SAME ``efin``.  The hard
                    # floor stays ``emin`` (``vfin >= vmin``).
                    cut_eval = min(max(h.efin, h.vmin), h.vmax)
                    reservoir["efin"] = cut_eval
                    reservoir["efin_cost"] = h.water_value
                else:
                    # No end-volume shipped → do NOT assume ``vfin = vini``.  The
                    # only hard end constraint is ``vfin >= vmin`` (the ``emin``
                    # bound at the last block/stage); the reservoir is still
                    # priced via its water value in the cut, which values the free
                    # terminal volume, linearised at the operating point ``eini``.
                    cut_eval = h.eini
                cuts.append((res_name, h.water_value, cut_eval))
            reservoir_array.append(reservoir)
            ruid += 1

        turbine_array.append(
            build_json_item(
                uid=tuid,
                name=f"tb_{_hy_ref(h.name)}",
                junction_a=jname,
                junction_b=junctions_by_code.get(h.downstream_code),
                generator=h.name,
                production_factor=h.fp_med if h.fp_med > 0.0 else None,
                capacity=h.qmax if h.qmax > 0.0 else None,
                # A committed-off unit (generator pmax 0) keeps passing water
                # downstream: ``drain`` makes the turbine power ≤ pf·flow (not
                # =), so flow > 0 with power 0 — the cascade isn't broken.
                drain=True if not h.committed else None,
            )
        )
        tuid += 1

        disch = (
            _inflow_sched(h.inflow_profile, study)
            if h.inflow_profile
            else (h.inflow if h.inflow > 0.0 else None)
        )
        if disch is not None:
            flow_array.append(
                {
                    "uid": fuid,
                    "name": f"in_{_hy_ref(h.name)}",
                    "junction": jname,
                    "direction": 1,
                    "discharge": disch,
                }
            )
            fuid += 1

    cut_csv = _build_boundary_cut_csv(cuts) if cuts else None
    return junction_array, reservoir_array, turbine_array, flow_array, cut_csv


def _build_multibus_planning(
    *,
    study: StudySpec,
    thermals: list[ThermalSpec],
    hydros: list[HydroSpec],
    demands: list[DemandSpec],
    buses: list[Any],
    circuits: list[Any],
    name: str,
    hydro_cost: float = 0.0,
    hydro_topology: bool = False,
) -> dict[str, Any]:
    """Assemble a multi-bus DC OPF planning from the PSR network entities."""
    live = {b.number for b in buses}
    # ``voltage`` = nominal kV from the PSR source data (Volt.dat is
    # authoritative; dbus.dat name suffix and highest-voltage-neighbour
    # inference fill the gaps — see dat_loader).  base_kv == 0 means
    # genuinely unknown, so the field is omitted rather than fabricated.
    bus_array: list[dict[str, Any]] = []
    for b in buses:
        bus_entry: dict[str, Any] = {"uid": b.number, "name": _net_bus_ref(b.number)}
        if b.base_kv > 0.0:
            bus_entry["voltage"] = b.base_kv
        bus_array.append(bus_entry)

    line_array: list[dict[str, Any]] = []
    uid = 1
    for c in circuits:
        if c.from_bus not in live or c.to_bus not in live:
            continue
        x = c.reactance_pu
        if abs(x) < _MIN_REACTANCE:
            x = _MIN_REACTANCE if x >= 0 else -_MIN_REACTANCE
        tmax = c.rating if c.rating > 0 else _UNLIMITED_RATING
        # PSR circuit names repeat (96 dups in the GUA case); the uid
        # suffix guarantees the unique Line name gtopt requires.
        label = (c.name or f"l{c.from_bus}_{c.to_bus}").replace(" ", "_")
        line_array.append(
            {
                "uid": uid,
                "name": f"{label}_{uid}",
                "bus_a": _net_bus_ref(c.from_bus),
                "bus_b": _net_bus_ref(c.to_bus),
                "reactance": x,
                "tmax_ab": tmax,
                "tmax_ba": tmax,
            }
        )
        uid += 1

    gen_array: list[dict[str, Any]] = []
    guid = 1
    for tplant in thermals:
        tbus = tplant.bus_number
        if tbus is None or tbus not in live or tplant.pmax <= 0.0:
            continue
        # pmax = the PSR cprmxtgu operational cap when present, else installed.
        cap = _pmax_cap(tplant.max_gen, study)
        # gcost = the PRECIOSMEX hourly bid when present (prices the imports),
        # else the scalar fuel-based cost.
        gprof = _pmax_cap(tplant.gcost_profile, study) if tplant.gcost_profile else None
        # No unit commitment in the base dispatch, so a forced pmin would
        # over-supply (free hydro becomes marginal); pmin relaxes to 0 (the LP
        # price duals match PSR's).  A RESTMEX (AMM) constraint, when present,
        # overrides the bounds (< pmax / > pmin / = fix).
        pmax_default = cap if cap is not None else tplant.pmax
        pmin_val, pmax_val = _amm_override(tplant, pmax_default, study)
        gen_array.append(
            {
                "uid": guid,
                "name": tplant.name or f"thermal_{tplant.code}",
                "bus": _net_bus_ref(tbus),
                "pmin": pmin_val,
                "pmax": pmax_val,
                "gcost": gprof if gprof is not None else _gen_gcost(tplant),
                "capacity": tplant.pmax,
            }
        )
        guid += 1
    dispatched_hydros: list[HydroSpec] = []
    for hplant in hydros:
        hbus = hplant.bus_number
        if hbus is None or hbus not in live or hplant.p_inst <= 0.0:
            continue
        # pmax = the PSR cprmxhgu operational cap (derating) when present, else
        # installed Pot — this is what holds hydro below installed in PSR.
        # A unit committed off (commith) is pinned to pmax 0 (it cannot
        # generate); its turbine still passes water downstream via ``drain``.
        cap = _pmax_cap(hplant.max_gen, study)
        gen_array.append(
            {
                "uid": guid,
                "name": hplant.name or f"hydro_{hplant.code}",
                "bus": _net_bus_ref(hbus),
                "pmin": 0.0,
                "pmax": 0.0
                if not hplant.committed
                else (cap if cap is not None else hplant.p_inst),
                # In full-topology mode the turbine + boundary cut govern hydro
                # dispatch, so generators are free (gcost 0).  Otherwise use the
                # per-plant water value (watervcp) or the --hydro-cost stand-in.
                "gcost": 0.0
                if hydro_topology
                else (hplant.gcost if hplant.gcost > 0 else hydro_cost),
                "capacity": hplant.p_inst,
            }
        )
        guid += 1
        dispatched_hydros.append(hplant)

    demand_array: list[dict[str, Any]] = []
    duid = 1
    for d in demands:
        dbus = d.bus_number
        if dbus is None or dbus not in live:
            continue
        demand_array.append(
            {
                "uid": duid,
                "name": d.name or f"dem_{dbus}",
                "bus": _net_bus_ref(dbus),
                "lmax": _normalize_demand_profile(d, study),
            }
        )
        duid += 1

    system: dict[str, Any] = {
        "name": name,
        "bus_array": bus_array,
        "generator_array": gen_array,
        "demand_array": demand_array,
        "line_array": line_array,
    }
    options = build_options(
        study,
        use_single_bus=False,
        use_kirchhoff=True,
        hydro_topology=hydro_topology,
    )

    cut_csv: str | None = None
    if hydro_topology and dispatched_hydros:
        junctions_by_code = {h.code: f"jn_{_hy_ref(h.name)}" for h in dispatched_hydros}
        jn, rs, tb, fl, cut_csv = _build_water_network(
            dispatched_hydros, junctions_by_code, study
        )
        system["junction_array"] = jn
        if rs:
            system["reservoir_array"] = rs
        system["turbine_array"] = tb
        if fl:
            system["flow_array"] = fl
        # Method follows the horizon: a single-stage week is a deterministic
        # dispatch and runs **monolithic** (like the plexos2gtopt cases); a
        # multi-stage horizon (``--blocks-per-stage``) runs **sddp/cascade**.
        # Either way the reservoir water value rides a boundary cut on the
        # reservoir end-volumes, wired into that method's options block.
        if cut_csv is not None:
            # The facade writes the CSV next to the JSON and rewrites the path
            # to absolute (resolve_input passes absolutes through unchanged).
            cut_opts = {
                "boundary_cuts_file": "boundary_cuts.csv",
                "boundary_cuts_mode": "combined",
            }
            if study.num_stages > 1:
                options["method"] = "sddp"
                options["sddp_options"] = cut_opts
            else:
                options["monolithic_options"] = cut_opts
        logger.info(
            "water network: %d junctions, %d reservoirs, %d turbines, %d inflows%s",
            len(jn),
            len(rs),
            len(tb),
            len(fl),
            f", {cut_csv.count(chr(10)) - 1} boundary cut" if cut_csv else "",
        )

    logger.info(
        "multi-bus planning '%s': %d buses, %d lines, %d generators, %d demands",
        name,
        len(bus_array),
        len(line_array),
        len(gen_array),
        len(demand_array),
    )
    planning: dict[str, Any] = {
        "options": options,
        "simulation": build_simulation(study),
        "system": system,
    }
    if cut_csv is not None:
        planning["_boundary_cuts"] = cut_csv
    return planning


def build_planning(
    *,
    study: StudySpec,
    systems: list[SystemSpec],
    thermals: list[ThermalSpec],
    hydros: list[HydroSpec],
    demands: list[DemandSpec],
    name: str,
    buses: list[Any] | None = None,
    circuits: list[Any] | None = None,
    hydro_cost: float = 0.0,
    hydro_topology: bool = False,
) -> dict[str, Any]:
    """Assemble the full gtopt planning JSON.

    Three modes:

    * **Multi-bus DC OPF** — when ``buses`` + ``circuits`` are supplied
      (PSR ``.dat`` network path): emits one ``Bus`` per ``dbus`` node,
      one ``Line`` per ``dcirc`` circuit (per-unit reactance), routes
      generators/demands to their ``bus_number``, and turns on Kirchhoff.
      With ``hydro_topology`` it additionally emits the full hydraulic
      network (junctions / reservoirs / turbines / inflow Flows) and a
      boundary cut carrying the per-reservoir water value.
    * **Single-system** — one bus, ``use_single_bus = true``.
    * **Multi-system** — one bus per ``PSRSystem`` (no inter-area links).

    ``hydro_cost`` is the uniform hydro opportunity cost [$/MWh] applied
    to every (zero-fuel) hydro generator — a stand-in for the PSR water
    value used only when ``hydro_topology`` is off.
    """
    if buses and circuits:
        return _build_multibus_planning(
            study=study,
            thermals=thermals,
            hydros=hydros,
            demands=demands,
            buses=buses,
            circuits=circuits,
            name=name,
            hydro_cost=hydro_cost,
            hydro_topology=hydro_topology,
        )

    if not systems:
        raise ValueError("build_planning: no PSRSystem entities")

    bus_by_ref, fallback_bus = _build_system_bus_map(systems)
    use_single_bus = len(systems) == 1

    # One aggregate bus per PSR System — a fictitious trade hub, not a
    # physical substation, so there is no nominal kV to emit here (the
    # multi-bus dbus.dat path above carries the real per-bus voltage).
    bus_array: list[dict[str, Any]] = [
        {"uid": i + 1, "name": _bus_name(s)} for i, s in enumerate(systems)
    ]

    gen_array: list[dict[str, Any]] = []
    gen_array.extend(
        build_thermal_generators(
            thermals, bus_by_ref=bus_by_ref, fallback_bus=fallback_bus
        )
    )
    gen_array.extend(
        build_hydro_generators(
            hydros,
            bus_by_ref=bus_by_ref,
            fallback_bus=fallback_bus,
            start_uid=len(gen_array) + 1,
            gcost=hydro_cost,
        )
    )
    demand_array = build_demands(
        demands, study, bus_by_ref=bus_by_ref, fallback_bus=fallback_bus
    )

    return {
        "options": build_options(study, use_single_bus=use_single_bus),
        "simulation": build_simulation(study),
        "system": {
            "name": name,
            "bus_array": bus_array,
            "generator_array": gen_array,
            "demand_array": demand_array,
        },
    }


# ``write_planning`` now lives in gtopt_shared.json_planning; re-exported
# here so existing ``from sddp2gtopt.gtopt_writer import write_planning``
# imports keep working.
