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
) -> dict[str, Any]:
    """Map :class:`StudySpec` onto gtopt's top-level ``options`` block.

    The 11 legacy top-level mirror keys (``use_single_bus`` /
    ``use_kirchhoff`` / ``demand_fail_cost`` / ``scale_objective`` / …)
    were moved into ``options.model_options.*`` by the 2026-05-17
    StrictParsePolicy migration; emitting them at top level now causes
    a JSON parse error.  Keep ``annual_discount_rate`` / ``output_*``
    at top level (those stayed) and nest the rest under
    ``model_options``.
    """
    return {
        "annual_discount_rate": study.discount_rate,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "model_options": {
            "use_single_bus": use_single_bus,
            "use_kirchhoff": use_kirchhoff,
            "demand_fail_cost": float(study.deficit_cost),
            "scale_objective": 1000,
        },
    }


def build_simulation(study: StudySpec) -> dict[str, Any]:
    """Map :class:`StudySpec` onto ``simulation`` (stages, blocks, scenarios).

    Each stage gets ``num_blocks`` sequential blocks, each of duration
    ``_stage_hours(stage_type) / num_blocks`` so the per-stage total
    duration matches the calendar.
    """
    total_hours = _stage_hours(study.stage_type)
    block_hours = total_hours / max(study.num_blocks, 1)
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
    return {
        "block_array": block_array,
        "stage_array": stage_array,
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    }


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
        return [list(demand.block_values)]

    block_hours = _stage_hours(study.stage_type) / max(study.num_blocks, 1)
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
) -> dict[str, Any]:
    """Assemble a multi-bus DC OPF planning from the PSR network entities."""
    live = {b.number for b in buses}
    bus_array = [{"uid": b.number, "name": _net_bus_ref(b.number)} for b in buses]

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
        gen_array.append(
            {
                "uid": guid,
                "name": tplant.name or f"thermal_{tplant.code}",
                "bus": _net_bus_ref(tbus),
                "pmin": tplant.pmin,
                "pmax": tplant.pmax,
                "gcost": _gen_gcost(tplant),
                "capacity": tplant.pmax,
            }
        )
        guid += 1
    for hplant in hydros:
        hbus = hplant.bus_number
        if hbus is None or hbus not in live or hplant.p_inst <= 0.0:
            continue
        gen_array.append(
            {
                "uid": guid,
                "name": hplant.name or f"hydro_{hplant.code}",
                "bus": _net_bus_ref(hbus),
                "pmin": 0.0,
                "pmax": hplant.p_inst,
                # Per-plant water value (from watervcp) when present,
                # else the uniform --hydro-cost stand-in.
                "gcost": hplant.gcost if hplant.gcost > 0 else hydro_cost,
                "capacity": hplant.p_inst,
            }
        )
        guid += 1

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

    logger.info(
        "multi-bus planning '%s': %d buses, %d lines, %d generators, %d demands",
        name,
        len(bus_array),
        len(line_array),
        len(gen_array),
        len(demand_array),
    )
    return {
        "options": build_options(study, use_single_bus=False, use_kirchhoff=True),
        "simulation": build_simulation(study),
        "system": {
            "name": name,
            "bus_array": bus_array,
            "generator_array": gen_array,
            "demand_array": demand_array,
            "line_array": line_array,
        },
    }


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
) -> dict[str, Any]:
    """Assemble the full gtopt planning JSON.

    Three modes:

    * **Multi-bus DC OPF** — when ``buses`` + ``circuits`` are supplied
      (PSR ``.dat`` network path): emits one ``Bus`` per ``dbus`` node,
      one ``Line`` per ``dcirc`` circuit (per-unit reactance), routes
      generators/demands to their ``bus_number``, and turns on Kirchhoff.
    * **Single-system** — one bus, ``use_single_bus = true``.
    * **Multi-system** — one bus per ``PSRSystem`` (no inter-area links).

    ``hydro_cost`` is the uniform hydro opportunity cost [$/MWh] applied
    to every (zero-fuel) hydro generator — a stand-in for the PSR water
    value (v0 has no reservoir coupling).
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
        )

    if not systems:
        raise ValueError("build_planning: no PSRSystem entities")

    bus_by_ref, fallback_bus = _build_system_bus_map(systems)
    use_single_bus = len(systems) == 1

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
