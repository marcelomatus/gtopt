# SPDX-License-Identifier: BSD-3-Clause
"""Per-entity snapshot tests for sddp2gtopt builders (issue #507 Phase 0).

Pins the JSON shape emitted by each ``build_*`` entity builder in
``sddp2gtopt.gtopt_writer`` for a minimal in-process spec.  These
complement the full-JSON golden in ``test_golden_round_trip.py`` by
giving drift attribution at the per-builder level — when a Phase 4
entity-builder refactor migrates one builder into the shared layer,
running just this test attributes any drift to the right entity.

Refresh with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …``.
"""

from __future__ import annotations

from pathlib import Path

from gtopt_shared.testing import assert_snapshot

from sddp2gtopt.entities import DemandSpec, HydroSpec, StudySpec, ThermalSpec
from sddp2gtopt.gtopt_writer import (
    build_demands,
    build_hydro_generators,
    build_thermal_generators,
)


_GOLDEN_DIR = Path(__file__).parent / "fixtures" / "entities"
_REFRESH_TARGET = "sddp2gtopt/tests/test_entity_snapshots.py"


def _assert_snapshot(name: str, payload) -> None:
    """Wrap the shared helper with this file's golden dir + refresh target."""
    assert_snapshot(name, payload, _GOLDEN_DIR, refresh_target=_REFRESH_TARGET)


def test_build_thermal_generators_snapshot() -> None:
    """Thermal builder emits ``{uid, name, bus, pmin, pmax, gcost, capacity}``."""
    plants = [
        ThermalSpec(
            code=1,
            name="T1",
            reference_id=101,
            pmin=10.0,
            pmax=100.0,
            g_segments=[(100.0, 42.5)],
            system_ref=1,
        ),
        ThermalSpec(
            code=2,
            name="T2",
            reference_id=102,
            pmin=0.0,
            pmax=50.0,
            g_segments=[(50.0, 60.0)],
            system_ref=2,
        ),
    ]
    out = build_thermal_generators(
        plants,
        bus_by_ref={1: "sys1_bus", 2: "sys2_bus"},
        fallback_bus="sys1_bus",
    )
    _assert_snapshot("build_thermal_generators", out)


def test_build_hydro_generators_snapshot() -> None:
    """Hydro builder flattens to zero-cost capped generator."""
    plants = [
        HydroSpec(
            code=10,
            name="H1",
            reference_id=210,
            p_inst=300.0,
            system_ref=1,
        ),
    ]
    out = build_hydro_generators(
        plants,
        bus_by_ref={1: "sys1_bus"},
        fallback_bus="sys1_bus",
        start_uid=3,
    )
    _assert_snapshot("build_hydro_generators", out)


def test_build_demands_snapshot() -> None:
    """Demand builder normalises ``GWh/stage`` to ``lmax MW/block`` matrix."""
    study = StudySpec(num_stages=3, num_blocks=2, stage_type=2)  # monthly
    demands = [
        DemandSpec(
            code=20,
            name="D1",
            reference_id=320,
            system_ref=1,
            profile=[100.0, 110.0, 120.0],
        ),
    ]
    out = build_demands(
        demands,
        study,
        bus_by_ref={1: "sys1_bus"},
        fallback_bus="sys1_bus",
    )
    _assert_snapshot("build_demands", out)
