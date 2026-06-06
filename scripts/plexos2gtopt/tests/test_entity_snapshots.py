# SPDX-License-Identifier: BSD-3-Clause
"""Per-entity snapshot tests for plexos2gtopt builders (issue #507 Phase 0).

Pins the JSON shape emitted by each ``build_*_array`` entity builder
in ``plexos2gtopt.gtopt_writer`` for a minimal in-process spec.
Complements the full-JSON golden in ``test_golden_round_trip.py`` by
giving drift attribution at the per-builder level — when a Phase 4
entity-builder refactor migrates one builder into the shared layer,
running just this test attributes any drift to the right entity.

This file covers **15 builders** with simple or moderately-rich
spec inputs.  The remaining 4 builders (turbine, user_constraint,
commitment, reserve_provision) have richer cross-referenced spec
trees with name-based generator / fuel / zone resolution; those
ship in a follow-up file once their fixture surface warrants its
own design.

Refresh with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …``.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from plexos2gtopt.entities import (
    BatterySpec,
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
    ReserveSpec,
    ReservoirSpec,
    WaterwaySpec,
)
from plexos2gtopt.gtopt_writer import (
    build_battery_array,
    build_bus_array,
    build_decision_variable_array,
    build_demand_array,
    build_emission_array,
    build_flow_array,
    build_flow_right_array,
    build_fuel_array,
    build_generator_array,
    build_junction_array,
    build_line_array,
    build_plant_array,
    build_reserve_zone_array,
    build_reservoir_array,
    build_waterway_array,
)


_GOLDEN_DIR = Path(__file__).parent / "fixtures" / "entities"


def _canonicalise(obj) -> str:
    """Return JSON sorted-key dump with stable indentation."""
    return json.dumps(obj, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


def _assert_snapshot(name: str, payload) -> None:
    """Compare payload against the named golden under ``fixtures/entities/``."""
    canonical = _canonicalise(payload)
    path = _GOLDEN_DIR / f"{name}.json"

    if os.environ.get("PYTEST_UPDATE_GOLDEN"):
        _GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        path.write_text(canonical, encoding="utf-8")
        pytest.skip(f"golden written to {path}")

    if not path.exists():
        pytest.skip(
            f"golden missing: {path}; create with "
            "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
            "plexos2gtopt/tests/test_entity_snapshots.py -q"
        )

    expected = path.read_text(encoding="utf-8")
    assert canonical == expected, (
        f"{name} entity output changed; if intentional, refresh with "
        "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
        "plexos2gtopt/tests/test_entity_snapshots.py -q"
    )


def test_build_bus_array_snapshot() -> None:
    """One bus entry per NodeSpec — just ``uid`` and ``name``."""
    nodes = (
        NodeSpec(object_id=1, name="NORTE"),
        NodeSpec(object_id=2, name="CENTRO", region="zone_A"),
        NodeSpec(object_id=3, name="SUR"),
    )
    _assert_snapshot("build_bus_array", build_bus_array(nodes))


def test_build_fuel_array_snapshot() -> None:
    """Fuel entries emit ``price``, optional CO₂ rows, and family tag."""
    fuels = (
        FuelSpec(
            object_id=10,
            name="diesel",
            price=85.0,
            heat_content=42.0,
            co2_rate=0.0741,
            type_tag="diesel",
        ),
        FuelSpec(object_id=11, name="biomasa", price=0.0, type_tag="biomasa"),
    )
    _assert_snapshot("build_fuel_array", build_fuel_array(fuels))


def test_build_emission_array_snapshot() -> None:
    """Emission entries derive from FuelSpecs with non-zero CO₂ rates."""
    fuels = (
        FuelSpec(
            object_id=10,
            name="diesel",
            price=85.0,
            co2_rate=0.0741,
            type_tag="diesel",
        ),
        FuelSpec(
            object_id=11,
            name="gas",
            price=30.0,
            co2_rate=0.0561,
            co2_upstream_rate=0.0030,
            type_tag="gas",
        ),
        # No CO₂ → should not emit an emission row.
        FuelSpec(object_id=12, name="biomasa", price=0.0, type_tag="biomasa"),
    )
    _assert_snapshot("build_emission_array", build_emission_array(fuels))


def test_build_junction_array_snapshot() -> None:
    """Junctions emit ``drain`` + optional ``drain_capacity``/``drain_cost``."""
    junctions = (
        JunctionSpec(name="LAJA"),
        JunctionSpec(
            name="ANGOSTURA",
            drain=True,
            drain_capacity=1500.0,
            drain_cost=7200.0,
        ),
    )
    _assert_snapshot("build_junction_array", build_junction_array(junctions))


def test_build_decision_variable_array_snapshot() -> None:
    """Decision vars emit bounds only when set; cost only when non-zero."""
    dvars = (
        DecisionVariableSpec(name="alpha_fcf", lower_bound=0.0, cost=1.0),
        DecisionVariableSpec(name="free_dv"),  # no bounds, no cost
        DecisionVariableSpec(name="bounded_dv", lower_bound=-50.0, upper_bound=100.0),
    )
    _assert_snapshot(
        "build_decision_variable_array", build_decision_variable_array(dvars)
    )


def test_build_plant_array_snapshot() -> None:
    """Plant entries emit generator member list + optional pmax/n_units/mutex."""
    plants = (
        PlantSpec(
            name="COCHRANE",
            generator_names=("COCHRANE_1", "COCHRANE_2"),
            pmax=550.0,
        ),
        PlantSpec(
            name="ANDINA",
            generator_names=("ANDINA_U1",),
            n_units=2,
            commit_coeffs=(1.0,),
            uniq_mutex=True,
        ),
    )
    _assert_snapshot("build_plant_array", build_plant_array(plants))


def test_build_demand_array_snapshot() -> None:
    """Demand entries emit ``lmax`` matrix + optional ``fcost`` override."""
    demands = (
        DemandSpec(
            name="ARICA_D",
            bus_name="ARICA",
            lmax_profile=(150.0, 140.0, 130.0, 145.0),
            fcost=467.19,
        ),
        DemandSpec(
            name="ANTOFAGASTA_D",
            bus_name="ANTOFAGASTA",
            lmax_profile=(220.0, 215.0, 210.0, 225.0),
        ),
    )
    _assert_snapshot("build_demand_array", build_demand_array(demands))


def test_build_battery_array_snapshot() -> None:
    """Battery entries emit energy bounds + power rating + efficiencies."""
    batteries = (
        BatterySpec(
            object_id=1,
            name="BAT_ARICA",
            bus_name="ARICA",
            emin=0.0,
            emax=2.0,
            eini=0.5,
            efin=0.5,
            pmax_charge=50.0,
            pmax_discharge=50.0,
            input_efficiency=0.92,
            output_efficiency=0.95,
            max_cycles_day=1.0,
        ),
    )
    _assert_snapshot("build_battery_array", build_battery_array(batteries))


def test_build_reservoir_array_snapshot() -> None:
    """Reservoir entries emit energy bounds + water value + optional profiles."""
    reservoirs = (
        ReservoirSpec(
            object_id=10,
            name="COCHRANE",
            emin=800.0,
            emax=2000.0,
            eini=1500.0,
            efin=1450.0,
            water_value=12.5,
        ),
        ReservoirSpec(
            object_id=11,
            name="LAJA",
            emin=100.0,
            emax=5000.0,
            eini=3000.0,
            efin=2900.0,
            water_value=1e30,
            never_drain=True,
            spill_penalty_per_mwh=1000.0,
        ),
    )
    _assert_snapshot("build_reservoir_array", build_reservoir_array(reservoirs))


def test_build_waterway_array_snapshot() -> None:
    """Waterway entries map junction_a/b + fmin/fmax + optional fcost."""
    waterways = (
        WaterwaySpec(
            object_id=20,
            name="Vert_PANGUE",
            storage_from="PANGUE",
            storage_to="ANGOSTURA",
            fmin=0.0,
            fmax=500.0,
            fcost=3.6,
        ),
        # waterway with no fcost — minimal entry
        WaterwaySpec(
            object_id=21,
            name="B_Maule",
            storage_from="MAULE",
            storage_to="LOMA_ALTA",
            fmax=1500.0,
        ),
    )
    _assert_snapshot("build_waterway_array", build_waterway_array(waterways))


def test_build_flow_array_snapshot() -> None:
    """Flow entries: inflow at a junction with optional discharge profile / fcost."""
    flows = (
        FlowSpec(
            name="inflow_COCHRANE",
            junction_name="COCHRANE",
            discharge_profile=(100.0, 105.0, 110.0),
        ),
        FlowSpec(
            name="slack_LAJA",
            junction_name="LAJA",
            fcost=10000.0,
        ),
    )
    _assert_snapshot("build_flow_array", build_flow_array(flows))


def test_build_reserve_zone_array_snapshot() -> None:
    """Reserve-zone entries emit ``urreq`` / ``drreq`` per-block matrices."""
    reserves = (
        ReserveSpec(
            object_id=30,
            name="SEN_RS",
            ur_requirement=(50.0,) * 24,
            ur_min_provision=10.0,
            urcost=14000.0,
            plexos_type=2,
            type_tag="spinning",
        ),
        ReserveSpec(
            object_id=31,
            name="SEN_LW",
            dr_requirement=(30.0,) * 24,
            dr_min_provision=5.0,
            drcost=8000.0,
            plexos_type=4,
            type_tag="regulation",
        ),
    )
    _assert_snapshot("build_reserve_zone_array", build_reserve_zone_array(reserves))


def test_build_flow_right_array_snapshot() -> None:
    """FlowRight entries with junction + optional fmin/fmax/target/bypass."""
    rights = (
        FlowRightSpec(
            name="Riego_LAJA_I",
            junction_name="LAJA",
            purpose="irrigation",
            fmin=15.0,
            fmax=50.0,
            target=30.0,
            fcost=720.0,
        ),
        # right with no junction → dropped by builder (logged once at parse time)
        FlowRightSpec(name="orphan_right", junction_name=None),
        FlowRightSpec(
            name="bypass_demo",
            junction_name="UPSTREAM",
            purpose="env_flow",
            bypass_junction="DOWNSTREAM",
            bypass_cost=5.0,
        ),
    )
    _assert_snapshot("build_flow_right_array", build_flow_right_array(rights))


def test_build_line_array_snapshot() -> None:
    """Line entries: bus_from/to, reactance, optional resistance + tmax_ba."""
    lines = (
        LineSpec(
            object_id=40,
            name="L_AB",
            bus_from="A",
            bus_to="B",
            tmax_ab=350.0,
            resistance=0.012,
        ),
    )
    _assert_snapshot("build_line_array", build_line_array(lines))


def test_build_generator_array_snapshot() -> None:
    """Generator entries: scalar gcost path with bus reference."""
    generators = (
        GeneratorSpec(
            object_id=50,
            name="COCHRANE_1",
            bus_name="COCHRANE",
            type_tag="thermal:coal",
            pmin=0.0,
            pmax=250.0,
            heat_rate=2.5,
            vom_charge=1.2,
            fuel_names=("COAL",),
        ),
        GeneratorSpec(
            object_id=51,
            name="SOLAR_NORTE",
            bus_name="NORTE",
            type_tag="renewable:solar",
            pmax=120.0,
            pmax_profile=(0.0, 0.0, 0.5, 0.9, 1.0, 0.8, 0.3, 0.0),
        ),
    )
    fuels = (FuelSpec(object_id=10, name="COAL", price=85.0, heat_content=24.0),)
    _assert_snapshot("build_generator_array", build_generator_array(generators, fuels))
