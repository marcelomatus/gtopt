"""Unit tests for :mod:`sddp2gtopt.parsers`."""

from __future__ import annotations

from pathlib import Path

import pytest

from sddp2gtopt.parsers import (
    _int,
    _scalar,
    parse_demands,
    parse_fuels,
    parse_gauging_stations,
    parse_hydro_plants,
    parse_study,
    parse_systems,
    parse_thermal_plants,
)
from sddp2gtopt.psrclasses_loader import PsrClassesLoader


@pytest.fixture
def case0(case0_dir: Path) -> PsrClassesLoader:
    return PsrClassesLoader.from_case_dir(case0_dir)


@pytest.fixture
def case_min(case_min_dir: Path) -> PsrClassesLoader:
    return PsrClassesLoader.from_case_dir(case_min_dir)


# --- helpers ----------------------------------------------------------


@pytest.mark.parametrize(
    ("val", "expected"),
    [
        (None, 0.0),
        ([], 0.0),
        ([3.5], 3.5),
        ([1, 2, 3], 1.0),
        (7, 7.0),
        (2.5, 2.5),
        ("nope", 0.0),
        ([{"x": 1}], 0.0),
    ],
)
def test_scalar_coercion(val: object, expected: float) -> None:
    assert _scalar(val) == expected


@pytest.mark.parametrize(
    ("val", "expected"),
    [(None, 0), ([], 0), ([5.4], 5), (12, 12), ("x", 0)],
)
def test_int_coercion(val: object, expected: int) -> None:
    assert _int(val) == expected


def test_scalar_default_is_used_when_missing() -> None:
    assert _scalar(None, default=42.0) == 42.0
    assert _scalar([], default=-1.0) == -1.0


# --- parse_study ------------------------------------------------------


def test_parse_study_case0(case0: PsrClassesLoader) -> None:
    s = parse_study(case0)
    assert s.initial_year == 2013
    assert s.initial_stage == 1
    assert s.stage_type == 2  # monthly
    assert s.num_stages == 2
    assert s.num_systems == 1
    assert s.num_blocks == 1
    assert s.deficit_cost == 500.0
    assert s.discount_rate == 0.0
    assert s.currency == "$"


def test_parse_study_min(case_min: PsrClassesLoader) -> None:
    s = parse_study(case_min)
    assert s.num_stages == 1
    assert s.num_blocks == 1
    assert s.deficit_cost == 500.0


def test_parse_study_falls_back_to_defaults(tmp_path: Path) -> None:
    """Empty ``PSRStudy`` array → dataclass defaults, no exceptions."""
    case = tmp_path / "psrclasses.json"
    case.write_text('{"PSRStudy": []}', encoding="utf-8")
    loader = PsrClassesLoader.from_file(case)
    s = parse_study(loader)
    assert s.initial_year == 2000
    assert s.num_stages == 1


# --- parse_systems ----------------------------------------------------


def test_parse_systems_case0(case0: PsrClassesLoader) -> None:
    systems = parse_systems(case0)
    assert len(systems) == 1
    sys0 = systems[0]
    assert sys0.code == 1
    assert sys0.name == "System 1"
    assert sys0.currency == "$"
    assert sys0.reference_id != 0


def test_parse_systems_two(
    case_two_systems_dir: Path,
) -> None:
    systems = parse_systems(PsrClassesLoader.from_case_dir(case_two_systems_dir))
    assert len(systems) == 2
    names = {s.name for s in systems}
    assert names == {"North", "South"}


# --- parse_fuels ------------------------------------------------------


def test_parse_fuels_case0(case0: PsrClassesLoader) -> None:
    fuels = parse_fuels(case0)
    assert len(fuels) == 2
    by_name = {f.name: f for f in fuels}
    assert by_name["Fuel 1"].cost == 0.8
    assert by_name["Fuel 2"].cost == 1.2
    assert by_name["Fuel 1"].unit == "MWh"


def test_parse_fuels_min(case_min: PsrClassesLoader) -> None:
    fuels = parse_fuels(case_min)
    assert len(fuels) == 1
    assert fuels[0].name == "Diesel"
    assert fuels[0].cost == 1.0


# --- parse_thermal_plants ---------------------------------------------


def test_parse_thermal_plants_case0(case0: PsrClassesLoader) -> None:
    plants = parse_thermal_plants(case0)
    assert len(plants) == 3
    by_name = {p.name: p for p in plants}
    # Cost = CEsp × FuelCost
    # T1: CEsp=10, fuel cost=0.8 → 8 $/MWh
    t1 = by_name["Thermal 1"]
    assert t1.pmax == 10.0
    assert t1.g_segments == [(100.0, pytest.approx(8.0))]
    # T2: CEsp=15, fuel 1 → 12
    assert by_name["Thermal 2"].g_segments == [(100.0, pytest.approx(12.0))]
    # T3: CEsp=12.5, fuel 2 cost=1.2 → 15
    assert by_name["Thermal 3"].g_segments == [(100.0, pytest.approx(15.0))]


def test_parse_thermal_plants_orphan_fuel(tmp_path: Path) -> None:
    """A plant with an unresolved fuel ref still parses (cost = 0)."""
    case = tmp_path / "psrclasses.json"
    case.write_text(
        '{"PSRThermalPlant": [{"name": "Orphan", "code": 1, '
        '"reference_id": 1, "GerMax": [10.0], '
        '"G(1)": [100.0], "CEsp(1,1)": [5.0], "fuels": [9999]}]}',
        encoding="utf-8",
    )
    plants = parse_thermal_plants(PsrClassesLoader.from_file(case))
    assert len(plants) == 1
    # Unresolved fuel → primary cost 0 → segment cost 0
    assert plants[0].g_segments == [(100.0, 0.0)]


def test_thermal_drops_zero_segments(case_min: PsrClassesLoader) -> None:
    """Segment 2/3 with cap 0 must be dropped, not emitted as (0, x)."""
    plants = parse_thermal_plants(case_min)
    assert len(plants[0].g_segments) == 1


# --- parse_hydro_plants -----------------------------------------------


def test_parse_hydro_plants_case0(case0: PsrClassesLoader) -> None:
    hydros = parse_hydro_plants(case0)
    assert len(hydros) == 1
    h = hydros[0]
    assert h.name == "Hydro 1"
    assert h.p_inst == 11.0
    assert h.vmin == 0.0
    assert h.vmax == 130.0
    assert h.qmax == 55.0
    assert h.fp_med == pytest.approx(0.2)


def test_parse_hydro_plants_thermal_only(
    case_thermal_only_dir: Path,
) -> None:
    """Thermal-only fixture has no PSRHydroPlant."""
    loader = PsrClassesLoader.from_case_dir(case_thermal_only_dir)
    assert not parse_hydro_plants(loader)


# --- parse_demands ----------------------------------------------------


def test_parse_demands_case0(case0: PsrClassesLoader) -> None:
    demands = parse_demands(case0)
    assert len(demands) == 1
    d = demands[0]
    assert d.name == "System 1"
    assert d.duracao_pct == 100.0
    # case0 demand profile: 12 monthly entries, first two non-zero
    assert len(d.profile) == 12
    assert d.profile[0] == pytest.approx(8.928)
    assert d.profile[1] == pytest.approx(8.064)


def test_parse_demands_no_segment(tmp_path: Path) -> None:
    """A demand without a matching DemandSegment yields an empty profile."""
    case = tmp_path / "psrclasses.json"
    case.write_text(
        '{"PSRDemand": [{"reference_id": 1, "code": 1, "name": "L1", '
        '"Duracao(1)": 100.0, "system": 99}]}',
        encoding="utf-8",
    )
    demands = parse_demands(PsrClassesLoader.from_file(case))
    assert len(demands) == 1
    assert demands[0].profile == []


# --- parse_gauging_stations -------------------------------------------


def test_parse_gauging_stations_case0(case0: PsrClassesLoader) -> None:
    stations = parse_gauging_stations(case0)
    assert len(stations) == 2
    # Both PSR sample stations carry a 480-element flat series
    for st in stations:
        assert len(st.vazao) == 480


def test_parse_gauging_stations_empty(case_min: PsrClassesLoader) -> None:
    assert not parse_gauging_stations(case_min)
