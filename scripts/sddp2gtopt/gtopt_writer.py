"""Build a gtopt JSON planning from parsed SDDP specs.

The output schema matches the small reference cases under
``cases/c0`` and ``cases/s1b`` — three top-level keys ``options``,
``simulation`` and ``system`` — so it can be solved by ``gtopt
--lp-only`` directly without further post-processing.

v0 produces a single-bus, monolithic LP. Multi-bus topology comes in
P3 once we parse ``ccombus*.dat``.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

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


def build_options(study: StudySpec, *, use_single_bus: bool = True) -> dict[str, Any]:
    """Map :class:`StudySpec` onto gtopt's top-level ``options`` block."""
    return {
        "annual_discount_rate": study.discount_rate,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "use_single_bus": use_single_bus,
        "demand_fail_cost": float(study.deficit_cost),
        "scale_objective": 1000,
        "use_kirchhoff": False,
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
    bus_name: str,
    start_uid: int = 1,
) -> list[dict[str, Any]]:
    """Map :class:`ThermalSpec` list onto gtopt ``generator_array`` entries."""
    out: list[dict[str, Any]] = []
    uid = start_uid
    for plant in plants:
        out.append(
            {
                "uid": uid,
                "name": plant.name or f"thermal_{plant.code}",
                "bus": bus_name,
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
    bus_name: str,
    start_uid: int,
) -> list[dict[str, Any]]:
    """Hydros are flattened to zero-cost generators in v0.

    Reservoir + turbine + inflow modelling is the P2 deliverable
    (see DESIGN.md). Until then we treat each hydro as a free-fuel
    generator capped at ``PotInst`` so the LP at least contains the
    right capacity.
    """
    out: list[dict[str, Any]] = []
    uid = start_uid
    for plant in plants:
        out.append(
            {
                "uid": uid,
                "name": plant.name or f"hydro_{plant.code}",
                "bus": bus_name,
                "pmin": 0.0,
                "pmax": plant.p_inst,
                "gcost": 0.0,
                "capacity": plant.p_inst,
            }
        )
        uid += 1
    return out


def _normalize_demand_profile(
    demand: DemandSpec, study: StudySpec
) -> list[list[float]]:
    """Build the ``lmax[stage][block]`` matrix from PSR's monthly series.

    PSR ``Demanda(1)`` is in **GWh per stage**. gtopt's ``lmax`` is in
    **MW per block**, so we divide by block duration in hours.
    """
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
    bus_name: str,
    start_uid: int = 1,
) -> list[dict[str, Any]]:
    """Map demands onto gtopt ``demand_array`` with inline ``lmax`` matrices."""
    out: list[dict[str, Any]] = []
    uid = start_uid
    for d in demands:
        out.append(
            {
                "uid": uid,
                "name": d.name or f"demand_{d.code}",
                "bus": bus_name,
                "lmax": _normalize_demand_profile(d, study),
            }
        )
        uid += 1
    return out


def build_planning(
    *,
    study: StudySpec,
    systems: list[SystemSpec],
    thermals: list[ThermalSpec],
    hydros: list[HydroSpec],
    demands: list[DemandSpec],
    name: str,
) -> dict[str, Any]:
    """Assemble the full gtopt planning JSON.

    Multi-system cases are not yet supported; the writer raises
    :class:`ValueError` to make the limitation visible at conversion
    time rather than letting gtopt produce an unsolvable model.
    """
    if len(systems) != 1:
        raise ValueError(
            f"sddp2gtopt v0 supports single-system cases only; "
            f"found {len(systems)} PSRSystem entities. "
            "Multi-system support is on the P4 roadmap (see DESIGN.md)."
        )
    bus = _bus_name(systems[0])

    gen_array: list[dict[str, Any]] = []
    gen_array.extend(build_thermal_generators(thermals, bus_name=bus))
    gen_array.extend(
        build_hydro_generators(hydros, bus_name=bus, start_uid=len(gen_array) + 1)
    )

    return {
        "options": build_options(study),
        "simulation": build_simulation(study),
        "system": {
            "name": name,
            "bus_array": [{"uid": 1, "name": bus}],
            "generator_array": gen_array,
            "demand_array": build_demands(demands, study, bus_name=bus),
        },
    }


def write_planning(planning: dict[str, Any], output_path: Path) -> Path:
    """Write the planning to ``output_path`` (parents created on demand)."""
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(planning, fh, indent=2)
        fh.write("\n")
    logger.info("wrote gtopt planning: %s", output_path)
    return output_path
