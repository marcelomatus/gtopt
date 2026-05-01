"""Unit tests for :mod:`sddp2gtopt.gtopt_writer`."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sddp2gtopt.entities import (
    DemandSpec,
    HydroSpec,
    StudySpec,
    SystemSpec,
    ThermalSpec,
)
from sddp2gtopt.gtopt_writer import (
    _stage_hours,
    build_demands,
    build_hydro_generators,
    build_options,
    build_planning,
    build_simulation,
    build_thermal_generators,
    write_planning,
)


def _system() -> SystemSpec:
    return SystemSpec(code=1, name="S1", reference_id=2000, currency="$")


def _study(num_stages: int = 1, num_blocks: int = 1) -> StudySpec:
    return StudySpec(num_stages=num_stages, num_blocks=num_blocks, deficit_cost=500)


# --- options -----------------------------------------------------------


def test_options_carry_study_settings() -> None:
    opts = build_options(_study())
    assert opts["demand_fail_cost"] == 500
    assert opts["use_single_bus"] is True
    assert opts["use_kirchhoff"] is False
    assert opts["scale_objective"] == 1000


def test_options_multi_bus_when_requested() -> None:
    opts = build_options(_study(), use_single_bus=False)
    assert opts["use_single_bus"] is False


# --- simulation --------------------------------------------------------


@pytest.mark.parametrize(
    ("stage_type", "expected"),
    [(1, 168.0), (2, 730.0), (3, 2190.0), (99, 730.0)],
)
def test_stage_hours_codes(stage_type: int, expected: float) -> None:
    assert _stage_hours(stage_type) == expected


def test_simulation_block_and_stage_arrays() -> None:
    sim = build_simulation(_study(num_stages=3, num_blocks=2))
    assert len(sim["stage_array"]) == 3
    assert len(sim["block_array"]) == 6
    # Each stage indexes a sequential pair of blocks
    assert sim["stage_array"][0]["first_block"] == 0
    assert sim["stage_array"][0]["count_block"] == 2
    assert sim["stage_array"][1]["first_block"] == 2
    assert sim["stage_array"][2]["first_block"] == 4
    # Block durations sum to one stage's hour budget
    block_hours = 730.0 / 2
    for blk in sim["block_array"]:
        assert blk["duration"] == pytest.approx(block_hours)


def test_simulation_single_scenario() -> None:
    sim = build_simulation(_study())
    assert sim["scenario_array"] == [{"uid": 1, "probability_factor": 1}]


# --- thermal generators -----------------------------------------------


def test_thermal_generators_use_cheapest_segment() -> None:
    plant = ThermalSpec(
        code=1,
        name="T1",
        reference_id=4000,
        pmax=20,
        g_segments=[(50, 30.0), (50, 20.0)],
    )
    gens = build_thermal_generators([plant], bus_name="b1")
    assert gens == [
        {
            "uid": 1,
            "name": "T1",
            "bus": "b1",
            "pmin": 0.0,
            "pmax": 20,
            "gcost": 20.0,
            "capacity": 20,
        }
    ]


def test_thermal_generator_uid_offset_works() -> None:
    plant = ThermalSpec(
        code=1, name="T", reference_id=1, pmax=1.0, g_segments=[(1.0, 1.0)]
    )
    gens = build_thermal_generators([plant], bus_name="b1", start_uid=42)
    assert gens[0]["uid"] == 42


def test_thermal_generator_default_name_when_blank() -> None:
    plant = ThermalSpec(code=7, name="", reference_id=1, pmax=1, g_segments=[(1, 1)])
    gens = build_thermal_generators([plant], bus_name="b1")
    assert gens[0]["name"] == "thermal_7"


def test_thermal_generator_zero_segments_yield_zero_cost() -> None:
    plant = ThermalSpec(code=1, name="T", reference_id=1, pmax=10)
    gens = build_thermal_generators([plant], bus_name="b1")
    assert gens[0]["gcost"] == 0.0


# --- hydro generators -------------------------------------------------


def test_hydro_generator_zero_cost_capped_by_pinst() -> None:
    plant = HydroSpec(code=1, name="H", reference_id=1, p_inst=12.0)
    gens = build_hydro_generators([plant], bus_name="b1", start_uid=5)
    assert gens == [
        {
            "uid": 5,
            "name": "H",
            "bus": "b1",
            "pmin": 0.0,
            "pmax": 12.0,
            "gcost": 0.0,
            "capacity": 12.0,
        }
    ]


# --- demands ----------------------------------------------------------


def test_demand_lmax_converts_gwh_to_mw() -> None:
    demand = DemandSpec(code=1, name="L", reference_id=1, profile=[7.30, 14.60])
    out = build_demands([demand], _study(num_stages=2), bus_name="b1")
    # 7.30 GWh / 730 h → 10 MW; 14.60 GWh / 730 h → 20 MW
    assert out[0]["lmax"] == [[10.0], [20.0]]


def test_demand_pads_with_zero_when_profile_is_short() -> None:
    demand = DemandSpec(code=1, name="L", reference_id=1, profile=[7.30])
    out = build_demands([demand], _study(num_stages=3), bus_name="b1")
    assert out[0]["lmax"] == [[10.0], [0.0], [0.0]]


def test_demand_default_name_when_blank() -> None:
    demand = DemandSpec(code=42, name="", reference_id=1, profile=[1.0])
    out = build_demands([demand], _study(), bus_name="b1")
    assert out[0]["name"] == "demand_42"


# --- build_planning ---------------------------------------------------


def test_build_planning_single_system_full_round_trip() -> None:
    plan = build_planning(
        study=_study(num_stages=2, num_blocks=1),
        systems=[_system()],
        thermals=[
            ThermalSpec(
                code=1,
                name="T1",
                reference_id=4000,
                pmax=10,
                g_segments=[(100.0, 8.0)],
            )
        ],
        hydros=[],
        demands=[
            DemandSpec(code=1, name="L1", reference_id=5000, profile=[7.30, 5.84])
        ],
        name="case_min",
    )
    assert plan["options"]["demand_fail_cost"] == 500
    assert len(plan["simulation"]["stage_array"]) == 2
    assert plan["system"]["name"] == "case_min"
    assert plan["system"]["bus_array"][0]["name"] == "sys_1_bus"
    assert len(plan["system"]["generator_array"]) == 1
    assert plan["system"]["demand_array"][0]["lmax"] == [[10.0], [8.0]]


def test_build_planning_multi_system_rejected() -> None:
    with pytest.raises(ValueError, match="single-system"):
        build_planning(
            study=_study(),
            systems=[_system(), _system()],
            thermals=[],
            hydros=[],
            demands=[],
            name="oops",
        )


def test_build_planning_includes_hydro_after_thermal() -> None:
    plan = build_planning(
        study=_study(),
        systems=[_system()],
        thermals=[
            ThermalSpec(
                code=1,
                name="T",
                reference_id=4000,
                pmax=10,
                g_segments=[(100.0, 8.0)],
            )
        ],
        hydros=[HydroSpec(code=1, name="H", reference_id=4001, p_inst=20)],
        demands=[],
        name="mix",
    )
    gens = plan["system"]["generator_array"]
    assert [g["name"] for g in gens] == ["T", "H"]
    assert [g["uid"] for g in gens] == [1, 2]


# --- write_planning ---------------------------------------------------


def test_write_planning_creates_parents(tmp_path: Path) -> None:
    out = tmp_path / "deep" / "tree" / "plan.json"
    plan = {"options": {}, "simulation": {}, "system": {}}
    write_planning(plan, out)
    assert out.is_file()
    assert json.loads(out.read_text(encoding="utf-8")) == plan
