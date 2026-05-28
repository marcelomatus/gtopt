# SPDX-License-Identifier: BSD-3-Clause
"""Tests for plexos2gtopt._comparison element/UC/indicator extraction."""

from __future__ import annotations

import dataclasses

import pytest

from gtopt_check_json._info import SystemCounts
from plexos2gtopt._comparison import (
    _log_comparison,
    _plexos_element_counts,
    _plexos_indicators,
    _plexos_uc_classification,
    compute_comparison_indicators,
    log_conversion_stats,
    run_post_check,
)
from plexos2gtopt.entities import (
    BatterySpec,
    BoundaryCutSpec,
    BundleSpec,
    CommitmentSpec,
    DemandSpec,
    FlowSpec,
    FuelSpec,
    GeneratorSpec,
    LineSpec,
    NodeSpec,
    PlexosCase,
    ReserveProvisionSpec,
    ReservoirSpec,
    TurbineSpec,
    UserConstraintSpec,
    UserConstraintStats,
)


def _make_case() -> PlexosCase:
    bundle = BundleSpec(
        bundle_date="2026-04-22", block_layout=((1, 2), (3, 4), (5, 6, 7, 8))
    )
    gens = (
        GeneratorSpec(
            object_id=1,
            name="COAL1",
            bus_name="b1",
            pmax=300.0,
            heat_rate=9.0,
            fuel_names=("coal",),
        ),
        GeneratorSpec(object_id=2, name="SOLAR1", bus_name="b1", pmax=100.0),
        GeneratorSpec(object_id=3, name="HYDRO1", bus_name="b2", pmax=200.0),
    )
    turbines = (TurbineSpec(generator_name="HYDRO1", reservoir_name="R1"),)
    demands = (
        DemandSpec(name="d1", bus_name="b1", lmax_profile=(400.0, 420.0, 450.0)),
    )
    lines = (
        LineSpec(
            object_id=9,
            name="L1",
            bus_from="b1",
            bus_to="b2",
            tmax_ab=150.0,
            units=1,
        ),
    )
    flows = (
        FlowSpec(name="f1", junction_name="j1", discharge_profile=(50.0, 55.0, 60.0)),
    )
    ucs = (
        UserConstraintSpec(name="CC1_Uniq", expression="x <= 1"),
        UserConstraintSpec(name="Gas_MaxOp_A", expression="y <= 5"),
        UserConstraintSpec(name="SD_sec", expression="z >= 1", active=False),
        UserConstraintSpec(name="ANTUCOmin", expression="w >= 2"),
    )
    stats = UserConstraintStats(
        raw_plexos_constraints=10,
        empty_lhs_dropped=4,
        base_emitted=4,
        hydro_synthesized=0,
        plant_cap_synthesized=0,
    )
    return PlexosCase(
        bundle=bundle,
        nodes=(NodeSpec(object_id=1, name="b1"), NodeSpec(object_id=2, name="b2")),
        generators=gens,
        turbines=turbines,
        demands=demands,
        lines=lines,
        flows=flows,
        reservoirs=(
            ReservoirSpec(object_id=1, name="R1"),
            ReservoirSpec(object_id=2, name="R2"),
        ),
        user_constraints=ucs,
        uc_stats=stats,
        boundary_cut=BoundaryCutSpec(fcf=1.0, slopes={"R1": 2.0, "R2": 3.0}),
    )


@pytest.fixture
def case() -> PlexosCase:
    return _make_case()


def test_plexos_element_counts(case: PlexosCase):
    pc = _plexos_element_counts(case)
    assert pc["gen_hydro"] == 1
    assert pc["gen_thermal"] == 1
    assert pc["gen_renewable"] == 1
    assert pc["buses"] == 2
    assert pc["generators"] == 3
    assert pc["flows"] == 1
    assert pc["boundary_cuts"] == 1
    # Both R1/R2 slopes map to bundle reservoirs → counted, none discarded.
    assert pc["boundary_state_variables"] == 2
    assert pc["boundary_state_variables_discarded"] == []
    assert pc["user_constraints"] == 4


def test_collapse_orphan_drain_outflows_drops_sink_keeps_visible_flow():
    """Orphan ``*_sink`` / ``*_ocean`` drain junctions whose only consumers
    are waterway / turbine ``junction_b`` refs are collapsed: those refs
    become outflow mode (drop junction_b) and the sink junction disappears.
    Mirrors the Vert_*-style "keep flow visible, drop the ocean junction"
    pattern enabled by the new Waterway.junction_b optional schema."""
    from plexos2gtopt.gtopt_writer import (  # noqa: PLC0415
        _collapse_orphan_drain_outflows,
    )

    system: dict = {
        "junction_array": [
            {"uid": 1, "name": "RES_A"},
            {"uid": 2, "name": "RES_A_spill_sink", "drain": True},
            {"uid": 3, "name": "RES_B_terminal_ocean", "drain": True},
            {"uid": 4, "name": "RES_C", "drain": True},  # real reservoir junction
        ],
        "waterway_array": [
            {
                "uid": 1,
                "name": "Vert_RES_A",
                "junction_a": "RES_A",
                "junction_b": "RES_A_spill_sink",
            },
            {
                "uid": 2,
                "name": "ww_to_real",
                "junction_a": "RES_A",
                "junction_b": "RES_C",
            },
        ],
        "turbine_array": [
            {
                "uid": 1,
                "name": "tur_terminal",
                "junction_a": "RES_B",
                "junction_b": "RES_B_terminal_ocean",
                "generator": 1,
            },
        ],
    }
    collapsed = _collapse_orphan_drain_outflows(system)
    assert collapsed == 2  # both *_sink and *_terminal_ocean
    names_left = {j["name"] for j in system["junction_array"]}
    assert "RES_A_spill_sink" not in names_left
    assert "RES_B_terminal_ocean" not in names_left
    assert "RES_C" in names_left  # real reservoir junction, not collapsed
    # Outflow mode: junction_b stripped from the converted entries.
    assert "junction_b" not in system["waterway_array"][0]
    assert system["waterway_array"][0]["junction_a"] == "RES_A"
    assert "junction_b" not in system["turbine_array"][0]
    # Real downstream still wired.
    assert system["waterway_array"][1]["junction_b"] == "RES_C"


def test_collapse_orphan_drain_outflows_skips_when_referenced_elsewhere():
    """A drain ``*_sink`` junction also referenced as a reservoir/flow
    junction is NOT collapsed — that other consumer pins it in place."""
    from plexos2gtopt.gtopt_writer import (  # noqa: PLC0415
        _collapse_orphan_drain_outflows,
    )

    system: dict = {
        "junction_array": [
            {"uid": 1, "name": "A"},
            {"uid": 2, "name": "shared_sink", "drain": True},
        ],
        "waterway_array": [
            {"uid": 1, "name": "ww1", "junction_a": "A", "junction_b": "shared_sink"},
        ],
        "flow_array": [
            {"uid": 1, "name": "f1", "junction": "shared_sink"},
        ],
    }
    collapsed = _collapse_orphan_drain_outflows(system)
    assert collapsed == 0
    assert any(j["name"] == "shared_sink" for j in system["junction_array"])
    assert system["waterway_array"][0]["junction_b"] == "shared_sink"


def test_collapse_orphan_drain_outflows_ignores_non_drain_and_non_sink_names():
    """Only ``drain=True`` junctions whose name ends in ``_sink`` / ``_ocean``
    are candidates — a non-drain or differently-named drain is left alone."""
    from plexos2gtopt.gtopt_writer import (  # noqa: PLC0415
        _collapse_orphan_drain_outflows,
    )

    system: dict = {
        "junction_array": [
            {"uid": 1, "name": "A"},
            # drain=True but no _sink/_ocean suffix → not a candidate
            {"uid": 2, "name": "PEHUENCHE", "drain": True},
            # _sink suffix but drain=False → not a candidate
            {"uid": 3, "name": "stale_sink"},
        ],
        "waterway_array": [
            {"uid": 1, "name": "ww1", "junction_a": "A", "junction_b": "PEHUENCHE"},
            {"uid": 2, "name": "ww2", "junction_a": "A", "junction_b": "stale_sink"},
        ],
    }
    collapsed = _collapse_orphan_drain_outflows(system)
    assert collapsed == 0
    assert len(system["junction_array"]) == 3


def test_plexos_boundary_state_vars_discards_non_bundle_reservoir():
    """A water-value slope for a reservoir absent from the bundle (e.g.
    PILMAIQUEN) is excluded from the comparable count and named in the
    discarded list — so the row ties out 1:1 with what the writer emits."""
    case = _make_case()
    case = dataclasses.replace(
        case,
        boundary_cut=BoundaryCutSpec(
            fcf=1.0, slopes={"R1": 2.0, "R2": 3.0, "PILMAIQUEN": 4.0}
        ),
    )
    pc = _plexos_element_counts(case)
    # PILMAIQUEN is not a bundle reservoir → not counted, listed as discarded.
    assert pc["boundary_state_variables"] == 2
    assert pc["boundary_state_variables_discarded"] == ["PILMAIQUEN"]


def test_plexos_uc_classification(case: PlexosCase):
    uc = _plexos_uc_classification(case)
    assert uc["by_family"] == {
        "config_exclusivity": 1,
        "gas_offtake": 1,
        "security": 1,
        "operational": 1,
    }
    assert uc["active"] == 3
    assert uc["inactive"] == 1
    assert uc["total"] == 4
    assert uc["raw_plexos_constraints"] == 10
    assert uc["empty_lhs_dropped"] == 4
    assert uc["base_emitted"] == 4


def test_plexos_indicators(case: PlexosCase):
    ind = _plexos_indicators(case)
    assert ind["total_gen_capacity_mw"] == 600.0
    assert ind["hydro_capacity_mw"] == 200.0
    assert ind["thermal_capacity_mw"] == 300.0
    assert ind["total_line_capacity_mw"] == 300.0
    assert ind["first_block_demand_mw"] == 400.0
    assert ind["last_block_demand_mw"] == 450.0
    # durations from block_layout are [2, 2, 4]; demand profile (400,420,450):
    assert ind["total_energy_mwh"] == 400 * 2 + 420 * 2 + 450 * 4
    assert ind["total_energy_mwh"] == 3440.0
    assert ind["first_block_affluent_avg"] == 50.0


def test_plexos_indicators_tier1_to_4() -> None:
    """Cost / commitment / storage / network indicators (Tiers 1-4)."""
    case = PlexosCase(
        bundle=BundleSpec(demand_fail_cost=500.0),
        generators=(
            GeneratorSpec(
                object_id=1,
                name="COAL1",
                bus_name="b1",
                pmax=300.0,
                heat_rate=10.0,
                vom_charge=2.0,
                fuel_names=("coal",),
            ),
            GeneratorSpec(object_id=2, name="SOLAR1", bus_name="b1", pmax=100.0),
        ),
        fuels=(FuelSpec(object_id=1, name="coal", price=4.0, max_offtake=1000.0),),
        commitments=(
            CommitmentSpec(
                generator_name="COAL1", startup_cost=50000.0, shutdown_cost=8000.0
            ),
        ),
        batteries=(
            BatterySpec(
                object_id=3,
                name="BAT1",
                bus_name="b1",
                emax=200.0,
                pmax_discharge=50.0,
                input_efficiency=0.95,
                output_efficiency=0.95,
            ),
        ),
        reservoirs=(
            ReservoirSpec(object_id=4, name="R1", emax=900.0, eini=600.0, efin=400.0),
        ),
        reserve_provisions=(
            ReserveProvisionSpec(generator_name="COAL1", urmax=30.0, drmax=20.0),
        ),
        lines=(
            LineSpec(
                object_id=9,
                name="L1",
                bus_from="b1",
                bus_to="b2",
                tmax_ab=150.0,
                resistance=0.05,
            ),
        ),
        boundary_cut=BoundaryCutSpec(fcf=1.16e9, slopes={"R1": 2.0}),
    )
    ind = _plexos_indicators(case)
    # Tier 1: effective gcost = price×heat_rate + VOM = 4×10 + 2 = 42 (coal); 0 (solar)
    assert ind["max_gcost"] == 42.0
    assert ind["avg_gcost"] == 21.0
    assert ind["avg_heat_rate"] == 10.0
    assert ind["total_fuel_offtake_cap"] == 1000.0
    assert ind["num_fuel_caps"] == 1.0
    # Tier 2
    assert ind["total_startup_cost"] == 50000.0
    assert ind["total_shutdown_cost"] == 8000.0
    assert ind["num_committable"] == 1.0
    # Tier 3
    assert ind["total_battery_energy_mwh"] == 200.0
    assert ind["total_battery_power_mw"] == 50.0
    assert ind["avg_battery_efficiency"] == pytest.approx(0.9025)
    assert ind["total_reservoir_storage"] == 900.0
    assert ind["total_reservoir_initial"] == 600.0
    assert ind["total_reservoir_final"] == 400.0
    assert ind["fcf_intercept"] == 1.16e9
    assert ind["num_water_value_reservoirs"] == 1.0
    # Tier 4
    assert ind["num_lossy_lines"] == 1.0
    assert ind["total_up_provision_mw"] == 30.0
    assert ind["total_dn_provision_mw"] == 20.0
    assert ind["demand_fail_cost"] == 500.0


def test_gtopt_indicators_match_plexos_on_richer_planning() -> None:
    """compute_indicators (gtopt authority) computes the same new fields."""
    planning = {
        "system": {
            "generator_array": [
                {
                    "name": "COAL1",
                    "type": "thermal",
                    "gcost": 2.0,
                    "heat_rate": 10.0,
                    "fuel": "coal",
                    "capacity": 300.0,
                },
            ],
            "fuel_array": [{"name": "coal", "price": 4.0, "max_offtake": 1000.0}],
            "commitment_array": [
                {"generator": "COAL1", "startup_cost": 50000.0, "shutdown_cost": 8000.0}
            ],
            "battery_array": [
                {
                    "name": "BAT1",
                    "emax": 200.0,
                    "pmax_discharge": 50.0,
                    "input_efficiency": 0.95,
                    "output_efficiency": 0.95,
                }
            ],
        },
        "simulation": {"block_array": [{"duration": 1.0}]},
        "options": {"model_options": {"demand_fail_cost": 500.0}},
    }
    from gtopt_check_json._info import compute_indicators  # noqa: PLC0415

    ind = compute_indicators(planning)
    assert ind.avg_gcost == 42.0  # 4×10 + 2
    assert ind.total_startup_cost == 50000.0
    assert ind.total_battery_energy_mwh == 200.0
    assert ind.demand_fail_cost == 500.0


def test_compute_comparison_indicators_returns_two_dicts(case: PlexosCase):
    planning = {
        "system": {"generator_array": [{"type": "thermal", "capacity": 300}]},
        "simulation": {"block_array": [{}]},
        "options": {},
    }
    plexos_ind, gtopt_ind = compute_comparison_indicators(case, planning, base_dir="")
    assert isinstance(plexos_ind, dict)
    assert isinstance(gtopt_ind, dict)
    assert "total_gen_capacity_mw" in plexos_ind
    assert "total_gen_capacity_mw" in gtopt_ind


def test_log_comparison_renders_without_raising(case: PlexosCase):
    g = SystemCounts(
        buses=2,
        lines=1,
        generators=4,
        demands=1,
        batteries=1,
        reservoirs=0,
        turbines=1,
        junctions=0,
        waterways=0,
        flows=1,
        reserve_zones=0,
        reserve_provisions=0,
        fuels=0,
        commitments=0,
        flow_rights=0,
        decision_variables=0,
        num_blocks=3,
        num_stages=1,
        num_scenarios=1,
        boundary_cuts=1,
        boundary_state_variables=2,
        gen_by_type={"hydro": 1, "thermal": 1, "renewable": 2},
        user_constraints_total=3,
        user_constraints_active=2,
        user_constraints_inactive=1,
        uc_by_family={"config_exclusivity": 1, "gas_offtake": 1, "operational": 1},
        uc_in_pampl=3,
    )
    # Must not raise.
    _log_comparison(
        _plexos_element_counts(case),
        g,
        _plexos_uc_classification(case),
        _plexos_indicators(case),
        _plexos_indicators(case),
    )


def test_run_post_check_is_callable():
    # Smoke: the orchestrator is importable and exposed.
    assert callable(run_post_check)


def test_log_conversion_stats_renders_without_raising():
    """The plp2gtopt-style 'Conversion Results' base table renders."""
    planning = {
        "system": {
            "name": "PCP",
            "bus_array": [{}, {}],
            "generator_array": [
                {"type": "thermal"},
                {"type": "thermal"},
                {"type": "renewable"},
            ],
            "demand_array": [{"fcost": 467.19}],
            "line_array": [{}],
            "fuel_array": [{}],
            "commitment_array": [{}],
            "user_constraint_array": [
                {"name": "Gas_MaxOp_A"},
                {"name": "SD_x", "active": False},
            ],
        },
        "simulation": {
            "block_array": [{"duration": 1.0}],
            "stage_array": [{}],
            "scenario_array": [{}],
        },
        "options": {"model_options": {"use_kirchhoff": True}},
    }
    # Must not raise; exercises the rich table build from compute_counts.
    log_conversion_stats(planning)
