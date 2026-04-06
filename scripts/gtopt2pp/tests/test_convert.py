"""Tests for gtopt2pp conversion."""

import json
from pathlib import Path

import pandapower as pp
import pytest

from gtopt2pp.convert import (
    _build_bus_ref_map,
    _build_demand_profile_map,
    _build_gen_profile_map,
    _get_bus_uid_to_idx,
    _get_reference_bus_uid,
    _pu_to_ohm,
    _resolve_field_sched,
    convert,
    convert_all_blocks,
    format_diagnostic,
    get_ac_opf_requirements,
    load_gtopt_case,
    run_diagnostic,
)

# ── Paths ─────────────────────────────────────────────────────────────────

_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLP_CASE_2Y = _CASES_DIR / "plp_case_2y"

# ── Minimal gtopt case (4 buses, 2 generators, 2 demands, 3 lines) ──────────

_MINIMAL_CASE: dict = {
    "options": {
        "use_kirchhoff": True,
        "use_single_bus": False,
        "demand_fail_cost": 1000,
        "scale_objective": 1000,
    },
    "simulation": {
        "block_array": [
            {"uid": 1, "duration": 1},
            {"uid": 2, "duration": 1},
        ],
        "stage_array": [
            {"uid": 1, "first_block": 0, "count_block": 2, "active": 1},
        ],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    },
    "system": {
        "name": "test_4b",
        "bus_array": [
            {"uid": 1, "name": "b1", "reference_theta": 0},
            {"uid": 2, "name": "b2"},
            {"uid": 3, "name": "b3"},
            {"uid": 4, "name": "b4"},
        ],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": 1,
                "pmin": 0,
                "pmax": 300,
                "gcost": 20,
                "capacity": 300,
            },
            {
                "uid": 2,
                "name": "g2",
                "bus": 2,
                "pmin": 0,
                "pmax": 200,
                "gcost": 35,
                "capacity": 200,
            },
        ],
        "demand_array": [
            {"uid": 1, "name": "d1", "bus": 3, "lmax": [[150, 180]]},
            {"uid": 2, "name": "d2", "bus": 4, "lmax": [[100, 120]]},
        ],
        "line_array": [
            {
                "uid": 1,
                "name": "l1_2",
                "bus_a": 1,
                "bus_b": 2,
                "reactance": 0.05,
                "tmax_ab": 200,
                "tmax_ba": 200,
            },
            {
                "uid": 2,
                "name": "l1_3",
                "bus_a": 1,
                "bus_b": 3,
                "reactance": 0.1,
                "tmax_ab": 150,
                "tmax_ba": 150,
            },
            {
                "uid": 3,
                "name": "l2_4",
                "bus_a": 2,
                "bus_b": 4,
                "reactance": 0.08,
                "tmax_ab": 100,
                "tmax_ba": 100,
            },
        ],
    },
}


# ── Case with string (name) bus references ───────────────────────────────────

_NAME_REF_CASE: dict = {
    "options": {},
    "simulation": {
        "block_array": [{"uid": 1, "duration": 1}],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    },
    "system": {
        "name": "name_ref_test",
        "bus_array": [
            {"uid": 1, "name": "b1", "reference_theta": 0},
            {"uid": 2, "name": "b2"},
        ],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": "b1",
                "pmax": 100,
                "gcost": 10,
                "capacity": 100,
            },
        ],
        "demand_array": [
            {"uid": 1, "name": "d1", "bus": "b2", "lmax": 50},
        ],
        "line_array": [
            {
                "uid": 1,
                "name": "l1_2",
                "bus_a": "b1",
                "bus_b": "b2",
                "reactance": 0.05,
                "tmax_ab": 100,
                "tmax_ba": 100,
            },
        ],
    },
}


class TestResolveFieldSched:
    """Test _resolve_field_sched for various FieldSched types."""

    def test_scalar(self) -> None:
        assert _resolve_field_sched(42.0, 0, 0) == 42.0
        assert _resolve_field_sched(0, 0, 0) == 0.0

    def test_flat_list(self) -> None:
        assert _resolve_field_sched([10, 20, 30], 0, 1) == 20.0
        assert _resolve_field_sched([10, 20, 30], 0, 0) == 10.0

    def test_nested_list(self) -> None:
        field = [[100, 200], [300, 400]]
        assert _resolve_field_sched(field, 0, 0) == 100.0
        assert _resolve_field_sched(field, 0, 1) == 200.0
        assert _resolve_field_sched(field, 1, 0) == 300.0
        assert _resolve_field_sched(field, 1, 1) == 400.0

    def test_none(self) -> None:
        assert _resolve_field_sched(None, 0, 0) is None

    def test_string_file_sched(self) -> None:
        assert _resolve_field_sched("profile.parquet", 0, 0) is None

    def test_out_of_range(self) -> None:
        assert _resolve_field_sched([10, 20], 0, 5) is None
        assert _resolve_field_sched([[10]], 5, 0) is None


class TestPuToOhm:
    """Test per-unit to Ohm conversion."""

    def test_basic(self) -> None:
        # z_base = 110^2 / 100 = 121.0
        result = _pu_to_ohm(0.05, 110.0, 100.0)
        assert result == pytest.approx(0.05 * 121.0)

    def test_zero(self) -> None:
        assert _pu_to_ohm(0.0, 110.0) == 0.0


class TestBusHelpers:
    """Test bus-related helper functions."""

    def test_bus_uid_to_idx(self) -> None:
        system = _MINIMAL_CASE["system"]
        mapping = _get_bus_uid_to_idx(system)
        assert mapping == {1: 0, 2: 1, 3: 2, 4: 3}

    def test_bus_ref_map_includes_names(self) -> None:
        system = _MINIMAL_CASE["system"]
        ref_map = _build_bus_ref_map(system)
        assert ref_map[1] == 0
        assert ref_map["b1"] == 0
        assert ref_map[4] == 3
        assert ref_map["b4"] == 3

    def test_reference_bus(self) -> None:
        system = _MINIMAL_CASE["system"]
        assert _get_reference_bus_uid(system) == 1

    def test_no_reference_bus(self) -> None:
        system = {"bus_array": [{"uid": 1, "name": "b1"}]}
        assert _get_reference_bus_uid(system) is None


class TestProfileMaps:
    """Test generator and demand profile building."""

    def test_gen_profile_map(self) -> None:
        system = {
            "generator_profile_array": [
                {"generator": 3, "profile": [[0.5, 0.8]]},
            ],
        }
        profiles = _build_gen_profile_map(system, 0, 0)
        assert profiles[3] == pytest.approx(0.5)
        profiles = _build_gen_profile_map(system, 0, 1)
        assert profiles[3] == pytest.approx(0.8)

    def test_demand_profile_map(self) -> None:
        system = {
            "demand_profile_array": [
                {"demand": 1, "profile": [[1.0, 0.9]]},
            ],
        }
        profiles = _build_demand_profile_map(system, 0, 1)
        assert profiles[1] == pytest.approx(0.9)

    def test_empty(self) -> None:
        assert not _build_gen_profile_map({}, 0, 0)
        assert not _build_demand_profile_map({}, 0, 0)


class TestConvert:
    """Test the main convert function (scenario/block use UIDs)."""

    def test_minimal_case_buses(self) -> None:
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        assert len(net.bus) == 4

    def test_minimal_case_generators(self) -> None:
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        # 1 ext_grid (ref bus) + 1 gen
        assert len(net.ext_grid) == 1
        assert len(net.gen) == 1

    def test_minimal_case_loads(self) -> None:
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        assert len(net.load) == 2
        loads = net.load.sort_values("name")
        assert loads.iloc[0]["p_mw"] == pytest.approx(150.0)
        assert loads.iloc[1]["p_mw"] == pytest.approx(100.0)

    def test_minimal_case_loads_block2(self) -> None:
        net = convert(_MINIMAL_CASE, scenario=1, block=2)
        loads = net.load.sort_values("name")
        assert loads.iloc[0]["p_mw"] == pytest.approx(180.0)
        assert loads.iloc[1]["p_mw"] == pytest.approx(120.0)

    def test_minimal_case_lines(self) -> None:
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        assert len(net.line) == 3

    def test_ext_grid_cost(self) -> None:
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        eg_costs = net.poly_cost[net.poly_cost["et"] == "ext_grid"]
        assert len(eg_costs) == 1
        assert eg_costs.iloc[0]["cp1_eur_per_mw"] == pytest.approx(20.0)

    def test_gen_cost(self) -> None:
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        gen_costs = net.poly_cost[net.poly_cost["et"] == "gen"]
        assert len(gen_costs) == 1
        assert gen_costs.iloc[0]["cp1_eur_per_mw"] == pytest.approx(35.0)

    def test_network_name(self) -> None:
        net = convert(_MINIMAL_CASE)
        assert net.name == "test_4b"

    def test_system_base_mva(self) -> None:
        net = convert(_MINIMAL_CASE)
        assert net.sn_mva == pytest.approx(100.0)

    def test_default_scenario_block(self) -> None:
        """convert() with defaults uses first scenario/block UIDs."""
        net = convert(_MINIMAL_CASE)
        assert len(net.bus) == 4

    def test_gcost_as_field_sched(self) -> None:
        """gcost can be a FieldSched (list), not just a scalar."""
        case = {
            "options": {},
            "simulation": {
                "block_array": [
                    {"uid": 1, "duration": 1},
                    {"uid": 2, "duration": 1},
                ],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "gcost_list_test",
                "bus_array": [
                    {"uid": 1, "name": "b1", "reference_theta": 0},
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 1,
                        "pmax": 100,
                        "gcost": [[10.0, 20.0]],
                        "capacity": 100,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 1, "lmax": 50},
                ],
                "line_array": [],
            },
        }
        net = convert(case, scenario=1, block=1)
        eg_costs = net.poly_cost[net.poly_cost["et"] == "ext_grid"]
        assert eg_costs.iloc[0]["cp1_eur_per_mw"] == pytest.approx(10.0)

        net2 = convert(case, scenario=1, block=2)
        eg_costs2 = net2.poly_cost[net2.poly_cost["et"] == "ext_grid"]
        assert eg_costs2.iloc[0]["cp1_eur_per_mw"] == pytest.approx(20.0)

    def test_invalid_block_uid(self) -> None:
        """convert() raises ValueError for an unknown block UID."""
        with pytest.raises(ValueError, match="block UID 999"):
            convert(_MINIMAL_CASE, scenario=1, block=999)

    def test_no_reference_theta_uses_first_generator_bus_as_ext_grid(self) -> None:
        """When no bus has reference_theta, first generator's bus becomes ext_grid."""
        case = {
            "options": {},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "no_ref_theta",
                "bus_array": [
                    {"uid": 1, "name": "b1"},  # no reference_theta
                    {"uid": 2, "name": "b2"},
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 1,
                        "pmax": 100,
                        "gcost": 20,
                        "capacity": 100,
                    },
                    {
                        "uid": 2,
                        "name": "g2",
                        "bus": 2,
                        "pmax": 50,
                        "gcost": 30,
                        "capacity": 50,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 2, "lmax": 40},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "l1_2",
                        "bus_a": 1,
                        "bus_b": 2,
                        "reactance": 0.05,
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                ],
            },
        }
        net = convert(case, scenario=1, block=1)
        # First generator (g1 on bus 0) should be ext_grid
        assert len(net.ext_grid) == 1
        assert net.ext_grid.iloc[0]["bus"] == 0
        # Second generator (g2 on bus 1) should be a regular gen
        assert len(net.gen) == 1
        assert net.gen.iloc[0]["bus"] == 1
        # Cost must be assigned to the ext_grid
        eg_costs = net.poly_cost[net.poly_cost["et"] == "ext_grid"]
        assert len(eg_costs) == 1
        assert eg_costs.iloc[0]["cp1_eur_per_mw"] == pytest.approx(20.0)

    def test_no_generators_adds_slack_ext_grid(self) -> None:
        """When there are no generators, an ext_grid is added on the first bus."""
        case = {
            "options": {},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "no_gens",
                "bus_array": [
                    {"uid": 1, "name": "b1"},
                    {"uid": 2, "name": "b2"},
                ],
                "generator_array": [],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 2, "lmax": 40},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "l1_2",
                        "bus_a": 1,
                        "bus_b": 2,
                        "reactance": 0.05,
                    },
                ],
            },
        }
        net = convert(case, scenario=1, block=1)
        # A fallback ext_grid must exist to avoid "no reference bus" errors
        assert len(net.ext_grid) == 1
        assert net.ext_grid.iloc[0]["bus"] == 0


class TestConvertNameReferences:
    """Test conversion with string (name) bus references."""

    def test_buses(self) -> None:
        net = convert(_NAME_REF_CASE, scenario=1, block=1)
        assert len(net.bus) == 2

    def test_generator_connected(self) -> None:
        net = convert(_NAME_REF_CASE, scenario=1, block=1)
        assert len(net.ext_grid) == 1

    def test_load_connected(self) -> None:
        net = convert(_NAME_REF_CASE, scenario=1, block=1)
        assert len(net.load) == 1

    def test_line_connected(self) -> None:
        net = convert(_NAME_REF_CASE, scenario=1, block=1)
        assert len(net.line) == 1


class TestConvertAllBlocks:
    """Test convert_all_blocks."""

    def test_returns_list(self) -> None:
        nets = convert_all_blocks(_MINIMAL_CASE, scenario=1)
        assert len(nets) == 2

    def test_different_loads(self) -> None:
        nets = convert_all_blocks(_MINIMAL_CASE, scenario=1)
        loads_b0 = nets[0].load.sort_values("name")
        loads_b1 = nets[1].load.sort_values("name")
        assert loads_b0.iloc[0]["p_mw"] == pytest.approx(150.0)
        assert loads_b1.iloc[0]["p_mw"] == pytest.approx(180.0)


class TestTransformerConversion:
    """Test transformer conversion."""

    def test_transformer_type(self) -> None:
        case = {
            "options": {},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "trafo_test",
                "bus_array": [
                    {"uid": 1, "name": "b1", "reference_theta": 0},
                    {"uid": 2, "name": "b2"},
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 1,
                        "pmax": 100,
                        "gcost": 10,
                        "capacity": 100,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 2, "lmax": 50},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "t1_2",
                        "bus_a": 1,
                        "bus_b": 2,
                        "reactance": 0.05,
                        "type": "transformer",
                        "tmax_ab": 100,
                        "tmax_ba": 100,
                    },
                ],
            },
        }
        net = convert(case)
        assert len(net.trafo) == 1
        assert len(net.line) == 0

    def test_different_voltage_levels_auto_transformer(self) -> None:
        """Lines between buses at significantly different voltage levels are
        automatically modelled as pandapower transformers even when 'type' is
        not set to 'transformer'."""
        case = {
            "options": {},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "auto_trafo_test",
                "bus_array": [
                    {
                        "uid": 1,
                        "name": "b1_hv",
                        "voltage": 220.0,
                        "reference_theta": 0,
                    },
                    {"uid": 2, "name": "b2_lv", "voltage": 110.0},
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 1,
                        "pmax": 200,
                        "gcost": 20,
                        "capacity": 200,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 2, "lmax": 100},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "xfmr_hv_lv",
                        "bus_a": 1,
                        "bus_b": 2,
                        "reactance": 0.1,
                        "tmax_ab": 200,
                        "tmax_ba": 200,
                        # type intentionally not set to "transformer"
                    },
                ],
            },
        }
        net = convert(case)
        # Must be modelled as a transformer, not a standard line
        assert len(net.trafo) == 1
        assert len(net.line) == 0

    def test_auto_transformer_hv_lv_assignment(self) -> None:
        """When bus_b has a higher voltage than bus_a, hv_bus must still be
        the high-voltage bus (pandapower requires vn_hv_kv >= vn_lv_kv)."""
        case = {
            "options": {},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "reversed_hv_lv",
                "bus_array": [
                    # bus_a is 110 kV (lower), bus_b is 220 kV (higher)
                    {"uid": 1, "name": "b1_lv", "voltage": 110.0},
                    {
                        "uid": 2,
                        "name": "b2_hv",
                        "voltage": 220.0,
                        "reference_theta": 0,
                    },
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 2,
                        "pmax": 200,
                        "gcost": 20,
                        "capacity": 200,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 1, "lmax": 100},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "xfmr_lv_hv",
                        "bus_a": 1,
                        "bus_b": 2,
                        "reactance": 0.1,
                    },
                ],
            },
        }
        net = convert(case)
        assert len(net.trafo) == 1
        assert len(net.line) == 0
        # pandapower transformer must have vn_hv_kv >= vn_lv_kv
        trafo = net.trafo.iloc[0]
        assert trafo["vn_hv_kv"] >= trafo["vn_lv_kv"]
        assert trafo["vn_hv_kv"] == pytest.approx(220.0)
        assert trafo["vn_lv_kv"] == pytest.approx(110.0)

    def test_near_threshold_100kv_110kv_auto_transformer(self) -> None:
        """A line between 100 kV and 110 kV buses (9.1% diff, above 5% threshold)
        must be auto-promoted to a transformer even though both are in the
        'medium voltage' range."""
        case = {
            "options": {},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "near_threshold_kv",
                "bus_array": [
                    {
                        "uid": 1,
                        "name": "b1_110",
                        "voltage": 110.0,
                        "reference_theta": 0,
                    },
                    {"uid": 2, "name": "b2_100", "voltage": 100.0},
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 1,
                        "pmax": 200,
                        "gcost": 20,
                        "capacity": 200,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 2, "lmax": 100},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "l_110_100",
                        "bus_a": 1,
                        "bus_b": 2,
                        "reactance": 0.1,
                    },
                ],
            },
        }
        net = convert(case)
        # 9.1% > 5% threshold → must become a transformer
        assert len(net.trafo) == 1
        assert len(net.line) == 0

    def test_same_voltage_stays_line(self) -> None:
        """Lines between buses at the same voltage level remain standard lines."""
        case = {
            "options": {},
            "simulation": {
                "block_array": [{"uid": 1, "duration": 1}],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "same_kv",
                "bus_array": [
                    {"uid": 1, "name": "b1", "voltage": 220.0, "reference_theta": 0},
                    {"uid": 2, "name": "b2", "voltage": 220.0},
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 1,
                        "pmax": 200,
                        "gcost": 20,
                        "capacity": 200,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 2, "lmax": 100},
                ],
                "line_array": [
                    {
                        "uid": 1,
                        "name": "l1_2",
                        "bus_a": 1,
                        "bus_b": 2,
                        "reactance": 0.05,
                    },
                ],
            },
        }
        net = convert(case)
        assert len(net.line) == 1
        assert len(net.trafo) == 0


class TestLoadGtoptCase:
    """Test load_gtopt_case."""

    def test_file_not_found(self) -> None:
        with pytest.raises(FileNotFoundError):
            load_gtopt_case("/nonexistent/path.json")

    def test_load_valid(self, tmp_path: Path) -> None:
        p = tmp_path / "test.json"
        p.write_text(json.dumps(_MINIMAL_CASE), encoding="utf-8")
        case = load_gtopt_case(p)
        assert case["system"]["name"] == "test_4b"


class TestACOPFRequirements:
    """Test get_ac_opf_requirements."""

    def test_returns_dict(self) -> None:
        reqs = get_ac_opf_requirements()
        assert isinstance(reqs, dict)
        assert "line.r_pu" in reqs
        assert "generator.min_q_mvar" in reqs
        assert "bus.min_vm_pu" in reqs


class TestGeneratorProfileConversion:
    """Test that generator profiles are applied during conversion."""

    def test_profile_applied(self) -> None:
        case = {
            "options": {},
            "simulation": {
                "block_array": [
                    {"uid": 1, "duration": 1},
                    {"uid": 2, "duration": 1},
                ],
                "scenario_array": [{"uid": 1, "probability_factor": 1}],
            },
            "system": {
                "name": "profile_test",
                "bus_array": [
                    {"uid": 1, "name": "b1", "reference_theta": 0},
                ],
                "generator_array": [
                    {
                        "uid": 1,
                        "name": "g1",
                        "bus": 1,
                        "pmax": 100,
                        "gcost": 10,
                        "capacity": 100,
                    },
                ],
                "demand_array": [
                    {"uid": 1, "name": "d1", "bus": 1, "lmax": 50},
                ],
                "line_array": [],
                "generator_profile_array": [
                    {"generator": 1, "profile": [[0.5, 0.8]]},
                ],
            },
        }
        net_b0 = convert(case, scenario=1, block=1)
        net_b1 = convert(case, scenario=1, block=2)

        # ext_grid max should be scaled by profile
        assert net_b0.ext_grid.iloc[0]["max_p_mw"] == pytest.approx(50.0)
        assert net_b1.ext_grid.iloc[0]["max_p_mw"] == pytest.approx(80.0)


# ── Diagnostic tests ─────────────────────────────────────────────────────────


class TestRunDiagnostic:
    """Test run_diagnostic and format_diagnostic."""

    def test_clean_network_no_issues(self) -> None:
        """A well-formed minimal case should produce no diagnostic issues."""
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        diag = run_diagnostic(net)
        # The minimal case is clean — no topology issues expected.
        # Some pandapower versions may report minor notes; the key checks
        # (different_voltage_levels_connected, disconnected_elements) must
        # be absent.
        assert "different_voltage_levels_connected" not in diag
        assert "disconnected_elements" not in diag

    def test_isolated_bus_detected(self) -> None:
        """A bus with no connections should be flagged by diagnostic."""
        case = json.loads(json.dumps(_MINIMAL_CASE))
        case["system"]["bus_array"].append({"uid": 99, "name": "b_isolated"})
        net = convert(case, scenario=1, block=1)
        diag = run_diagnostic(net)
        assert "disconnected_elements" in diag

    def test_format_diagnostic_empty(self) -> None:
        """Formatting an empty diagnostic dict should say no issues."""
        assert format_diagnostic({}) == "No issues found."

    def test_format_diagnostic_with_issues(self) -> None:
        """Formatting a diagnostic dict with issues should include check names."""
        diag = {"disconnected_elements": [4]}
        report = format_diagnostic(diag)
        assert "disconnected_elements" in report
        assert "4" in report

    def test_format_diagnostic_dict_values(self) -> None:
        """Formatting handles dict-typed diagnostic values."""
        diag = {"some_check": {"bus": [1, 2], "line": [3]}}
        report = format_diagnostic(diag)
        assert "some_check" in report
        assert "bus" in report

    def test_format_diagnostic_scalar_values(self) -> None:
        """Formatting handles scalar diagnostic values."""
        diag = {"some_check": "a scalar message"}
        report = format_diagnostic(diag)
        assert "a scalar message" in report


class TestCLIDiagnostic:
    """Test the --diagnostic CLI flag."""

    def test_diagnostic_flag_prints_report(
        self, tmp_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """--diagnostic should print the diagnostic section."""
        from gtopt2pp.main import main  # pylint: disable=import-outside-toplevel

        p = tmp_path / "case.json"
        p.write_text(json.dumps(_MINIMAL_CASE), encoding="utf-8")
        rc = main(["--no-check", "--diagnostic", str(p)])
        assert rc == 0
        captured = capsys.readouterr()
        assert "pandapower diagnostic" in captured.out

    def test_no_diagnostic_flag_omits_report(
        self, tmp_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Without --diagnostic the report should not appear."""
        from gtopt2pp.main import main  # pylint: disable=import-outside-toplevel

        p = tmp_path / "case.json"
        p.write_text(json.dumps(_MINIMAL_CASE), encoding="utf-8")
        rc = main(["--no-check", str(p)])
        assert rc == 0
        captured = capsys.readouterr()
        assert "pandapower diagnostic" not in captured.out


# ── Integration tests (require pandapower DC OPF) ───────────────────────────


@pytest.mark.integration
class TestDCOPFSolve:
    """Integration tests that actually run pandapower DC OPF."""

    def test_minimal_case_solves(self) -> None:
        """Verify that the minimal 4-bus case solves successfully."""
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        pp.rundcopp(net)
        assert net.OPF_converged

    def test_minimal_case_generation_meets_demand(self) -> None:
        """Total generation should match total demand."""
        net = convert(_MINIMAL_CASE, scenario=1, block=1)
        pp.rundcopp(net)
        total_gen = net.res_ext_grid["p_mw"].sum() + net.res_gen["p_mw"].sum()
        total_load = net.load["p_mw"].sum()
        assert total_gen == pytest.approx(total_load, abs=0.1)


# ── plp_case_2y end-to-end integration test ──────────────────────────────────


@pytest.mark.slow
@pytest.mark.integration
@pytest.mark.skipif(not _PLP_CASE_2Y.exists(), reason="plp_case_2y case not present")
class TestPlpCase2YGtopt2PP:
    """Integration test: plp_case_2y → plp2gtopt → gtopt2pp → pandapower.

    Validates the full conversion pipeline for a real Chilean SEN network:
    1.  ``plp2gtopt`` converts the PLP dat-files to a gtopt JSON case.
    2.  ``gtopt2pp`` converts the JSON to a pandapower network.
    3.  The network topology is sound (buses, lines, trafos).
    4.  Lines between buses at different voltage levels are auto-promoted to
        pandapower transformers (no ``different_voltage_levels_connected``
        pandapower diagnostic warning).
    5.  A pandapower DC OPF can be executed on the network.
    """

    @pytest.fixture(scope="class")
    def gtopt_case(self, tmp_path_factory: pytest.TempPathFactory) -> dict:
        """Convert plp_case_2y (stage 1 only) to a gtopt JSON case dict."""
        from plp2gtopt.plp2gtopt import (  # pylint: disable=import-outside-toplevel
            convert_plp_case,
        )

        tmp = tmp_path_factory.mktemp("plp_case_2y")
        out_dir = tmp / "gtopt_case_2y"
        out_dir.mkdir(parents=True, exist_ok=True)
        opts = {
            "input_dir": _PLP_CASE_2Y,
            "output_dir": out_dir,
            "output_file": tmp / "gtopt_case_2y.json",
            # Use only stage 1 so the fixture completes quickly
            "last_stage": 1,
            "hydrologies": "all",
            "num_apertures": "all",
            "last_time": -1,
            "compression": "gzip",
            "probability_factors": None,
            "discount_rate": 0.0,
            "management_factor": 0.0,
        }
        convert_plp_case(opts)
        with open(tmp / "gtopt_case_2y.json", encoding="utf-8") as fh:
            case = json.load(fh)
        case["_case_dir"] = str(out_dir)
        return case

    @pytest.fixture(scope="class")
    def pp_net(self, gtopt_case: dict) -> pp.pandapowerNet:
        """Convert the gtopt case to a pandapower network (first scenario, first block)."""
        scenarios = gtopt_case["simulation"]["scenario_array"]
        first_uid = scenarios[0]["uid"]
        blocks = gtopt_case["simulation"]["block_array"]
        first_block = blocks[0]["uid"]
        return convert(gtopt_case, scenario=first_uid, block=first_block)

    # ── structural assertions ────────────────────────────────────────────────

    def test_gtopt_case_has_buses(self, gtopt_case: dict) -> None:
        """Converted gtopt case must have buses."""
        buses = gtopt_case["system"]["bus_array"]
        assert len(buses) > 0

    def test_gtopt_case_has_multiple_voltage_levels(self, gtopt_case: dict) -> None:
        """plp_case_2y spans several voltage levels (66/110/154/220/345/500 kV)."""
        voltages = {b.get("voltage") for b in gtopt_case["system"]["bus_array"]}
        voltages.discard(None)
        assert len(voltages) > 3, (
            f"Expected at least 4 distinct voltage levels, got {voltages}"
        )

    def test_gtopt_case_has_lines(self, gtopt_case: dict) -> None:
        """Converted gtopt case must have transmission lines."""
        assert len(gtopt_case["system"]["line_array"]) > 0

    def test_pandapower_network_has_buses(self, pp_net: pp.pandapowerNet) -> None:
        """pandapower network must have buses after conversion."""
        assert len(pp_net.bus) > 0

    def test_pandapower_network_has_transformers(
        self, pp_net: pp.pandapowerNet
    ) -> None:
        """Auto-detection must produce pandapower transformers for cross-voltage lines."""
        assert len(pp_net.trafo) > 0, (
            "Expected at least one transformer from cross-voltage-level lines"
        )

    def test_no_different_voltage_levels_connected(
        self, pp_net: pp.pandapowerNet
    ) -> None:
        """After auto-promotion of cross-voltage lines to transformers, pandapower
        must not flag 'different_voltage_levels_connected'."""
        diag = pp.diagnostic(pp_net, report_style=None)
        assert "different_voltage_levels_connected" not in diag, (
            "Lines between different-voltage buses should be transformers, "
            f"not standard lines. Issues: {diag.get('different_voltage_levels_connected')}"
        )

    def test_pandapower_network_has_generators(self, pp_net: pp.pandapowerNet) -> None:
        """pandapower network must have generators."""
        total_gens = len(pp_net.gen) + len(pp_net.ext_grid)
        assert total_gens > 0

    def test_pandapower_network_has_loads(self, pp_net: pp.pandapowerNet) -> None:
        """pandapower network must have loads."""
        assert len(pp_net.load) > 0

    def test_pandapower_network_line_count(self, pp_net: pp.pandapowerNet) -> None:
        """Total branches (lines + trafos) is positive."""
        total_branches = len(pp_net.line) + len(pp_net.trafo)
        assert total_branches > 0

    # ── DC OPF ────────────────────────────────────────────────────────────────

    def test_pandapower_dc_opf_runs(self, pp_net: pp.pandapowerNet) -> None:
        """pandapower rundcopp must complete without raising an exception.

        For a large real-world case the DC OPF may not converge (singular
        bus-admittance matrix due to very high reactances); that is
        acceptable.  What is *not* acceptable is a crash (unexpected
        exception such as TypeError, AttributeError, etc.).
        """
        import copy  # pylint: disable=import-outside-toplevel

        from pandapower.optimal_powerflow import (  # pylint: disable=import-outside-toplevel
            OPFNotConverged,
        )

        net_copy = copy.deepcopy(pp_net)
        try:
            pp.rundcopp(net_copy)
        except OPFNotConverged:
            pass  # non-convergence on a large real-world case is expected
        # OPF_converged attribute must be present regardless of convergence
        assert hasattr(net_copy, "OPF_converged")
