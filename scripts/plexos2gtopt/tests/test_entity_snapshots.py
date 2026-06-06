# SPDX-License-Identifier: BSD-3-Clause
"""Per-entity snapshot tests for plexos2gtopt builders (issue #507 Phase 0).

Pins the JSON shape emitted by each ``build_*_array`` entity builder
in ``plexos2gtopt.gtopt_writer`` for a minimal in-process spec.
Complements the full-JSON golden in ``test_golden_round_trip.py`` by
giving drift attribution at the per-builder level — when a Phase 4
entity-builder refactor migrates one builder into the shared layer,
running just this test attributes any drift to the right entity.

This file covers the **simple-spec** builders (bus, fuel, emission,
junction, decision_variable, plant).  Richer builders that require
NetworkSpec / GeneratorSpec / LineSpec / ReservoirSpec / etc. trees
(line, generator, demand, battery, reservoir, waterway, turbine,
flow, reserve_zone, user_constraint, flow_right, commitment,
reserve_provision) ship in a follow-up file as their input surface
warrants its own fixture set.

Refresh with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …``.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from plexos2gtopt.entities import (
    DecisionVariableSpec,
    FuelSpec,
    JunctionSpec,
    NodeSpec,
    PlantSpec,
)
from plexos2gtopt.gtopt_writer import (
    build_bus_array,
    build_decision_variable_array,
    build_emission_array,
    build_fuel_array,
    build_junction_array,
    build_plant_array,
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
