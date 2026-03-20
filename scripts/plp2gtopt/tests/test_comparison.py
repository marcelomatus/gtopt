"""Tests for the PLP vs gtopt element comparison formatting."""

from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any
from unittest.mock import MagicMock, patch

import pytest

from plp2gtopt._comparison import (
    _extract_flow_central_names,
    _gtopt_element_counts,
    _gtopt_indicators,
    _plp_active_hydrology_indices,
    _plp_element_counts,
)
from plp2gtopt.plp2gtopt import (
    _cc,
    _delta_str,
    _log_comparison,
    _use_color,
    _vis_len,
)


# ---------------------------------------------------------------------------
# Helper utilities
# ---------------------------------------------------------------------------


def test_vis_len_plain() -> None:
    """_vis_len returns the visible character count for plain text."""
    assert _vis_len("hello") == 5
    assert _vis_len("") == 0


def test_vis_len_ansi() -> None:
    """_vis_len strips ANSI escape codes before measuring."""
    coloured = "\033[1m\033[92mhello\033[0m"
    assert _vis_len(coloured) == 5


def test_cc_with_colour() -> None:
    """_cc wraps text in ANSI codes when *use_color* is True."""
    result = _cc("\033[1m", "title", use_color=True)
    assert result.startswith("\033[1m")
    assert result.endswith("\033[0m")
    assert "title" in result


def test_cc_without_colour() -> None:
    """_cc returns plain text when *use_color* is False."""
    result = _cc("\033[1m", "title", use_color=False)
    assert result == "title"


def test_use_color_false_without_tty() -> None:
    """_use_color returns False when no handler streams to a terminal."""
    assert _use_color() is False  # test runners are not a tty


# ---------------------------------------------------------------------------
# Delta formatting
# ---------------------------------------------------------------------------


def test_delta_str_match() -> None:
    """_delta_str shows check-mark for equal values."""
    result = _delta_str(10, 10, use_color=False)
    assert "✓" in result
    assert len(result) == 6


def test_delta_str_negative() -> None:
    """_delta_str shows negative delta when gtopt < plp."""
    result = _delta_str(100, 80, use_color=False)
    assert "-20" in result


def test_delta_str_positive() -> None:
    """_delta_str shows positive delta when gtopt > plp."""
    result = _delta_str(5, 12, use_color=False)
    assert "+7" in result


def test_delta_str_none() -> None:
    """_delta_str returns blanks when either value is None."""
    result = _delta_str(None, 10, use_color=False)
    assert result.strip() == ""
    result2 = _delta_str(10, None, use_color=False)
    assert result2.strip() == ""


def test_delta_str_colour() -> None:
    """_delta_str includes ANSI codes when use_color is True."""
    result = _delta_str(10, 10, use_color=True)
    assert "\033[" in result
    assert "✓" in result


# ---------------------------------------------------------------------------
# _gtopt_element_counts
# ---------------------------------------------------------------------------


def test_gtopt_element_counts_empty() -> None:
    """Empty planning dict returns all-zero counts."""
    counts = _gtopt_element_counts({})
    assert counts["buses"] == 0
    assert counts["generators"] == 0
    assert counts["turbines"] == 0
    assert counts["scenarios"] == 0


def test_gtopt_element_counts_populated() -> None:
    """Planning dict with arrays returns correct lengths."""
    planning = {
        "system": {
            "bus_array": [{"uid": 1}, {"uid": 2}],
            "generator_array": [
                {"uid": 1, "type": "termica"},
                {"uid": 2, "type": "embalse"},
                {"uid": 3, "type": "termica"},
            ],
            "generator_profile_array": [],
            "demand_array": [{"uid": 1}, {"uid": 2}, {"uid": 3}],
            "demand_profile_array": [],
            "line_array": [],
            "battery_array": [{"uid": 1}],
            "converter_array": [],
            "junction_array": [{"uid": 1}],
            "waterway_array": [],
            "flow_array": [],
            "reservoir_array": [],
            "reservoir_efficiency_array": [{"uid": 1}],
            "filtration_array": [],
            "turbine_array": [{"uid": 1}, {"uid": 2}],
        },
        "simulation": {
            "block_array": [{"uid": 1}] * 24,
            "stage_array": [{"uid": 1}],
            "scenario_array": [{"uid": 1}] * 3,
        },
    }
    counts = _gtopt_element_counts(planning)
    assert counts["buses"] == 2
    assert counts["generators"] == 3
    assert counts["demands"] == 3
    assert counts["batteries"] == 1
    assert counts["turbines"] == 2
    assert counts["blocks"] == 24
    assert counts["scenarios"] == 3
    # Generator type breakdown
    assert counts["gen_termica"] == 2
    assert counts["gen_embalse"] == 1
    # Reservoir efficiencies
    assert counts["reservoir_efficiencies"] == 1


def test_gtopt_element_counts_missing_type() -> None:
    """Generators with missing type attribute are counted as 'unknown'."""
    planning = {
        "system": {
            "generator_array": [
                {"uid": 1, "type": "termica"},
                {"uid": 2},  # no type attribute
                {"uid": 3, "type": ""},  # empty string type
            ],
        },
        "simulation": {},
    }
    counts = _gtopt_element_counts(planning)
    assert counts["generators"] == 3
    assert counts["gen_termica"] == 1
    assert counts["gen_unknown"] == 1
    assert counts.get("gen_", 0) == 1  # empty string lowered


# ---------------------------------------------------------------------------
# _log_comparison — integration test via stderr capture
# ---------------------------------------------------------------------------


def test_log_comparison_output(capsys: pytest.CaptureFixture) -> None:
    """_log_comparison produces the expected table sections."""
    plp = {
        "buses": 10,
        "lines": 5,
        "centrals": 30,
        "sub_embalse": 3,
        "sub_serie": 7,
        "sub_pasada": 5,
        "sub_termica": 10,
        "sub_bateria": 2,
        "sub_falla": 3,
        "demands": 8,
        "batteries": 2,
        "blocks": 24,
        "stages": 1,
    }
    gtopt = {
        "buses": 10,
        "generators": 24,
        "generator_profiles": 5,
        "demands": 8,
        "demand_profiles": 0,
        "lines": 5,
        "batteries": 2,
        "converters": 0,
        "junctions": 12,
        "waterways": 15,
        "flows": 4,
        "reservoirs": 3,
        "filtrations": 1,
        "turbines": 8,
        "blocks": 24,
        "stages": 1,
        "scenarios": 3,
    }

    _log_comparison(plp, gtopt)

    output = capsys.readouterr().err

    # Section headers present
    assert "Network & Generation" in output
    assert "Hydro System" in output
    assert "Storage" in output
    assert "Simulation" in output

    # Demands are now under "Network & Generation", not a separate "Loads"
    assert "demands" in output

    # Key comparison header
    assert "PLP" in output
    assert "gtopt" in output

    # Derived rows
    assert "gen (excl falla+bat)" in output
    assert "hydro centrals (emb+ser)" in output

    # Analysis notes
    assert "batteries" in output
    assert "gtopt demands (unserved energy)" in output
    assert "only bus>0 with generation waterway" in output

    # Check that the embalse → reservoir match note appears
    assert "embalse count" in output

    # Check that pasada → generator profiles note appears
    assert "pasada" in output


def test_log_comparison_gen_delta(capsys: pytest.CaptureFixture) -> None:
    """When gen (excl falla+bat) != generators, the delta note appears."""
    plp = {
        "buses": 5,
        "lines": 3,
        "centrals": 20,
        "sub_embalse": 2,
        "sub_serie": 3,
        "sub_pasada": 4,
        "sub_termica": 6,
        "sub_bateria": 1,
        "sub_falla": 4,
        "demands": 5,
        "batteries": 1,
        "blocks": 10,
        "stages": 1,
    }
    gtopt = {
        "buses": 5,
        "generators": 13,  # 20 - 4 (falla) - 1 (bat) = 15 expected → delta -2
        "generator_profiles": 4,
        "demands": 5,
        "demand_profiles": 0,
        "lines": 3,
        "batteries": 1,
        "converters": 0,
        "junctions": 5,
        "waterways": 8,
        "flows": 2,
        "reservoirs": 2,
        "filtrations": 0,
        "turbines": 4,
        "blocks": 10,
        "stages": 1,
        "scenarios": 1,
    }

    _log_comparison(plp, gtopt)

    output = capsys.readouterr().err
    assert "centrals with bus<=0" in output


def test_log_comparison_hydrology_row(capsys: pytest.CaptureFixture) -> None:
    """The comparison table shows hydrologies vs scenarios row."""
    plp = {
        "buses": 5,
        "lines": 3,
        "centrals": 10,
        "sub_embalse": 0,
        "sub_serie": 0,
        "sub_pasada": 0,
        "sub_termica": 8,
        "sub_bateria": 0,
        "sub_falla": 2,
        "demands": 5,
        "batteries": 0,
        "blocks": 10,
        "stages": 1,
        "hydrologies": 16,
    }
    gtopt = {
        "buses": 5,
        "generators": 8,
        "generator_profiles": 0,
        "demands": 5,
        "lines": 3,
        "batteries": 0,
        "junctions": 0,
        "waterways": 0,
        "flows": 0,
        "reservoirs": 0,
        "reservoir_efficiencies": 0,
        "filtrations": 0,
        "turbines": 0,
        "blocks": 10,
        "stages": 1,
        "scenarios": 16,
    }

    _log_comparison(plp, gtopt)

    output = capsys.readouterr().err
    assert "hydrologies / scenarios" in output


def test_log_comparison_filtrations_res_eff(
    capsys: pytest.CaptureFixture,
) -> None:
    """The comparison table shows filtrations and reservoir efficiencies."""
    plp = {
        "buses": 3,
        "lines": 2,
        "centrals": 5,
        "sub_embalse": 2,
        "sub_serie": 1,
        "sub_pasada": 0,
        "sub_termica": 2,
        "sub_bateria": 0,
        "sub_falla": 0,
        "demands": 3,
        "batteries": 0,
        "blocks": 10,
        "stages": 1,
        "filtrations": 3,
        "reservoir_efficiencies": 2,
    }
    gtopt = {
        "buses": 3,
        "generators": 5,
        "generator_profiles": 0,
        "demands": 3,
        "lines": 2,
        "batteries": 0,
        "junctions": 3,
        "waterways": 5,
        "flows": 2,
        "reservoirs": 2,
        "reservoir_efficiencies": 2,
        "filtrations": 3,
        "turbines": 3,
        "blocks": 10,
        "stages": 1,
        "scenarios": 1,
    }

    _log_comparison(plp, gtopt)

    output = capsys.readouterr().err
    assert "reservoir efficiencies" in output
    assert "filtrations" in output


def test_log_comparison_demand_delta(capsys: pytest.CaptureFixture) -> None:
    """When demands differ, the delta note explains the exclusions."""
    plp = {
        "buses": 5,
        "lines": 2,
        "centrals": 3,
        "sub_embalse": 0,
        "sub_serie": 0,
        "sub_pasada": 0,
        "sub_termica": 3,
        "sub_bateria": 0,
        "sub_falla": 0,
        "demands": 10,
        "batteries": 0,
        "blocks": 5,
        "stages": 1,
    }
    gtopt = {
        "buses": 5,
        "generators": 3,
        "generator_profiles": 0,
        "demands": 7,
        "lines": 2,
        "batteries": 0,
        "junctions": 0,
        "waterways": 0,
        "flows": 0,
        "reservoirs": 0,
        "reservoir_efficiencies": 0,
        "filtrations": 0,
        "turbines": 0,
        "blocks": 5,
        "stages": 1,
        "scenarios": 1,
    }

    _log_comparison(plp, gtopt)

    output = capsys.readouterr().err
    assert "falla with bus>0:" in output or "PLP plpdem.dat" in output


def test_log_comparison_with_indicators(
    capsys: pytest.CaptureFixture,
) -> None:
    """Global indicators section appears when indicator dicts are provided."""
    plp = {
        "buses": 5,
        "lines": 2,
        "centrals": 5,
        "sub_embalse": 0,
        "sub_serie": 0,
        "sub_pasada": 0,
        "sub_termica": 5,
        "sub_bateria": 0,
        "sub_falla": 0,
        "demands": 5,
        "batteries": 0,
        "blocks": 5,
        "stages": 1,
    }
    gtopt = {
        "buses": 5,
        "generators": 5,
        "generator_profiles": 0,
        "demands": 5,
        "lines": 2,
        "batteries": 0,
        "junctions": 0,
        "waterways": 0,
        "flows": 0,
        "reservoirs": 0,
        "reservoir_efficiencies": 0,
        "filtrations": 0,
        "turbines": 0,
        "blocks": 5,
        "stages": 1,
        "scenarios": 1,
    }

    plp_ind = {
        "total_gen_capacity_mw": 500.0,
        "first_block_demand_mw": 200.0,
        "last_block_demand_mw": 180.0,
        "total_energy_mwh": 5000.0,
        "first_block_affluent_avg": 100.0,
        "last_block_affluent_avg": 90.0,
    }
    gtopt_ind = {
        "total_gen_capacity_mw": 500.0,
        "first_block_demand_mw": 200.0,
        "last_block_demand_mw": 180.0,
        "total_energy_mwh": 5000.0,
        "first_block_affluent_avg": 100.0,
        "last_block_affluent_avg": 90.0,
    }

    _log_comparison(plp, gtopt, plp_ind, gtopt_ind)

    output = capsys.readouterr().err
    assert "Global Indicators" in output
    assert "gen capacity (MW)" in output
    assert "first block demand (MW)" in output
    assert "total energy (TWh)" in output
    assert "capacity adequacy" in output
    # Verify capacity adequacy appears before affluent (moved next to energy)
    adequacy_pos = output.index("capacity adequacy")
    if "first block affluent" in output:
        affluent_pos = output.index("first block affluent")
        assert adequacy_pos < affluent_pos


# ---------------------------------------------------------------------------
# Helper: mock PLPParser factory
# ---------------------------------------------------------------------------


def _make_parser(parsed_data: dict[str, Any]) -> MagicMock:
    """Return a MagicMock that behaves like PLPParser with given parsed_data."""
    parser = MagicMock()
    parser.parsed_data = parsed_data
    return parser


# ---------------------------------------------------------------------------
# _plp_element_counts
# ---------------------------------------------------------------------------


class TestPlpElementCounts:
    """Tests for _plp_element_counts."""

    def test_empty_parsed_data(self) -> None:
        """Empty parsed_data returns an empty counts dict."""
        parser = _make_parser({})
        counts = _plp_element_counts(parser)
        assert not counts

    def test_bus_count(self) -> None:
        """Bus parser contributes buses count."""
        bus_p = SimpleNamespace(num_buses=7)
        parser = _make_parser({"bus_parser": bus_p})
        counts = _plp_element_counts(parser)
        assert counts["buses"] == 7

    def test_central_counts_with_types(self) -> None:
        """Central parser contributes total centrals and sub-type counts."""
        central_p = SimpleNamespace(
            num_centrals=5,
            centrals_of_type={
                "termica": [{"name": "t1"}, {"name": "t2"}],
                "embalse": [{"name": "e1"}],
                "serie": [{"name": "s1"}, {"name": "s2"}],
            },
            centrals=[],
        )
        parser = _make_parser({"central_parser": central_p})
        counts = _plp_element_counts(parser)
        assert counts["centrals"] == 5
        assert counts["sub_termica"] == 2
        assert counts["sub_embalse"] == 1
        assert counts["sub_serie"] == 2

    def test_demand_line_battery_counts(self) -> None:
        """Demand, line, and battery parsers contribute their counts."""
        parser = _make_parser(
            {
                "demand_parser": SimpleNamespace(num_demands=4),
                "line_parser": SimpleNamespace(num_lines=3),
                "battery_parser": SimpleNamespace(batteries=[{}, {}]),
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["demands"] == 4
        assert counts["lines"] == 3
        assert counts["batteries"] == 2

    def test_ess_count(self) -> None:
        """ESS parser contributes ess count from items list."""
        parser = _make_parser(
            {
                "ess_parser": SimpleNamespace(items=[{}, {}, {}]),
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["ess"] == 3

    def test_block_and_stage_counts(self) -> None:
        """Block and stage parsers contribute their counts."""
        parser = _make_parser(
            {
                "block_parser": SimpleNamespace(num_blocks=24),
                "stage_parser": SimpleNamespace(num_stages=12),
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["blocks"] == 24
        assert counts["stages"] == 12

    def test_filtrations_from_filemb(self) -> None:
        """Filemb parser takes priority for filtration count."""
        parser = _make_parser(
            {
                "filemb_parser": SimpleNamespace(num_filtrations=5),
                "cenfi_parser": SimpleNamespace(num_filtrations=3),
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["filtrations"] == 5

    def test_filtrations_from_cenfi_fallback(self) -> None:
        """Cenfi parser is used when filemb is absent."""
        parser = _make_parser(
            {
                "cenfi_parser": SimpleNamespace(num_filtrations=3),
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["filtrations"] == 3

    def test_reservoir_efficiencies(self) -> None:
        """Cenre parser contributes reservoir efficiency count."""
        parser = _make_parser(
            {
                "cenre_parser": SimpleNamespace(num_efficiencies=4),
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["reservoir_efficiencies"] == 4

    def test_stateless_reservoirs(self) -> None:
        """Embalse centrals with hid_indep=True are counted as stateless."""
        central_p = SimpleNamespace(
            num_centrals=3,
            centrals_of_type={
                "embalse": [
                    {"name": "e1", "hid_indep": True},
                    {"name": "e2", "hid_indep": False},
                    {"name": "e3", "hid_indep": True},
                ],
            },
            centrals=[],
        )
        parser = _make_parser({"central_parser": central_p})
        counts = _plp_element_counts(parser)
        assert counts["stateless_reservoirs"] == 2

    def test_hydrologies_and_affluents(self) -> None:
        """Aflce parser contributes hydrology count and affluent names."""
        central_p = SimpleNamespace(
            num_centrals=2,
            centrals_of_type={
                "embalse": [{"name": "hydro_a"}],
                "serie": [{"name": "hydro_b"}],
            },
            centrals=[],
        )
        central_p.get_central_by_name = {
            "hydro_a": {"type": "embalse"},
            "hydro_b": {"type": "serie"},
            "non_hydro": {"type": "termica"},
        }.get

        aflce_p = SimpleNamespace(
            items=[{"num_hydrologies": 10}],
            flows=[
                {"name": "hydro_a"},
                {"name": "hydro_b"},
                {"name": "non_hydro"},
            ],
        )
        parser = _make_parser(
            {
                "aflce_parser": aflce_p,
                "central_parser": central_p,
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["hydrologies"] == 10
        assert counts["affluents"] == 2
        assert sorted(counts["_affluent_names"]) == ["hydro_a", "hydro_b"]

    def test_affluents_without_central_parser(self) -> None:
        """Without central_parser, all aflce flows are counted as affluents."""
        aflce_p = SimpleNamespace(
            items=[{"num_hydrologies": 5}],
            flows=[{"name": "a"}, {"name": "b"}, {"name": "c"}],
        )
        parser = _make_parser({"aflce_parser": aflce_p})
        counts = _plp_element_counts(parser)
        assert counts["affluents"] == 3

    def test_missing_attributes_default_to_zero(self) -> None:
        """Parsers without expected attributes default to 0."""
        parser = _make_parser(
            {
                "bus_parser": SimpleNamespace(),  # no num_buses attr
                "demand_parser": SimpleNamespace(),  # no num_demands attr
            }
        )
        counts = _plp_element_counts(parser)
        assert counts["buses"] == 0
        assert counts["demands"] == 0


# ---------------------------------------------------------------------------
# _extract_flow_central_names
# ---------------------------------------------------------------------------


class TestExtractFlowCentralNames:
    """Tests for _extract_flow_central_names."""

    def test_empty_planning(self) -> None:
        """Returns None when planning has no flow_array."""
        assert _extract_flow_central_names({}) is None

    def test_empty_flow_array(self) -> None:
        """Returns None when flow_array is empty."""
        planning: dict[str, Any] = {"system": {"flow_array": []}}
        assert _extract_flow_central_names(planning) is None

    def test_flows_without_plp_central(self) -> None:
        """Returns None when no flow has a plp_central field."""
        planning: dict[str, Any] = {
            "system": {
                "flow_array": [
                    {"name": "flow_1"},
                    {"name": "flow_2"},
                ],
            },
        }
        assert _extract_flow_central_names(planning) is None

    def test_flows_with_plp_central(self) -> None:
        """Returns the set of plp_central names."""
        planning: dict[str, Any] = {
            "system": {
                "flow_array": [
                    {"name": "flow_1", "plp_central": "CenA"},
                    {"name": "flow_2", "plp_central": "CenB"},
                    {"name": "flow_3", "plp_central": "CenA"},  # duplicate
                ],
            },
        }
        result = _extract_flow_central_names(planning)
        assert result == {"CenA", "CenB"}

    def test_mixed_plp_central_fields(self) -> None:
        """Flows with empty/missing plp_central are skipped."""
        planning: dict[str, Any] = {
            "system": {
                "flow_array": [
                    {"name": "f1", "plp_central": "A"},
                    {"name": "f2", "plp_central": ""},
                    {"name": "f3"},
                    {"name": "f4", "plp_central": None},
                    {"name": "f5", "plp_central": "B"},
                ],
            },
        }
        result = _extract_flow_central_names(planning)
        assert result == {"A", "B"}


# ---------------------------------------------------------------------------
# _gtopt_element_counts — additional coverage
# ---------------------------------------------------------------------------


class TestGtoptElementCountsExtended:
    """Additional tests for _gtopt_element_counts edge cases."""

    def test_stateless_reservoirs(self) -> None:
        """Reservoirs with use_state_variable=False are counted."""
        planning: dict[str, Any] = {
            "system": {
                "reservoir_array": [
                    {"uid": 1, "use_state_variable": False},
                    {"uid": 2, "use_state_variable": True},
                    {"uid": 3, "use_state_variable": False},
                    {"uid": 4},  # missing attribute
                ],
                "generator_array": [],
            },
            "simulation": {},
        }
        counts = _gtopt_element_counts(planning)
        assert counts["stateless_reservoirs"] == 2

    def test_flow_names_combined(self) -> None:
        """_flow_names merges flow_array and generator_profile_array names."""
        planning: dict[str, Any] = {
            "system": {
                "flow_array": [
                    {"name": "flow_x"},
                    {"name": "flow_y"},
                ],
                "generator_profile_array": [
                    {"name": "prof_a"},
                ],
                "generator_array": [],
            },
            "simulation": {},
        }
        counts = _gtopt_element_counts(planning)
        assert counts["_flow_names"] == ["flow_x", "flow_y", "prof_a"]

    def test_converters_and_junctions(self) -> None:
        """Converter and junction arrays are counted."""
        planning: dict[str, Any] = {
            "system": {
                "converter_array": [{"uid": 1}],
                "junction_array": [{"uid": 1}, {"uid": 2}],
                "generator_array": [],
            },
            "simulation": {},
        }
        counts = _gtopt_element_counts(planning)
        assert counts["converters"] == 1
        assert counts["junctions"] == 2


# ---------------------------------------------------------------------------
# _gtopt_indicators — mock compute_indicators
# ---------------------------------------------------------------------------


class TestGtoptIndicators:
    """Tests for _gtopt_indicators with mocked compute_indicators."""

    def test_field_mapping(self) -> None:
        """All SystemIndicators fields are correctly mapped to the dict."""

        @dataclass
        class FakeIndicators:
            """Minimal stand-in for SystemIndicators."""

            total_gen_capacity_mw: float = 100.0
            hydro_capacity_mw: float = 40.0
            thermal_capacity_mw: float = 60.0
            total_line_capacity_mw: float = 200.0
            first_block_demand_mw: float = 80.0
            last_block_demand_mw: float = 70.0
            total_energy_mwh: float = 5000.0
            avg_annual_energy_mwh: float = 43800.0
            first_block_affluent_avg: float = 15.0
            last_block_affluent_avg: float = 12.0
            total_water_volume_hm3: float = 300.0
            avg_flow_m3s: float = 25.0

        fake = FakeIndicators()
        with patch(
            "gtopt_check_json._info.compute_indicators",
            return_value=fake,
        ) as mock_ci:
            result = _gtopt_indicators({"system": {}}, base_dir="/tmp/case")
            mock_ci.assert_called_once_with({"system": {}}, base_dir="/tmp/case")

        assert result["total_gen_capacity_mw"] == 100.0
        assert result["hydro_capacity_mw"] == 40.0
        assert result["thermal_capacity_mw"] == 60.0
        assert result["total_line_capacity_mw"] == 200.0
        assert result["first_block_demand_mw"] == 80.0
        assert result["last_block_demand_mw"] == 70.0
        assert result["total_energy_mwh"] == 5000.0
        assert result["avg_annual_energy_mwh"] == 43800.0
        assert result["first_block_affluent_avg"] == 15.0
        assert result["last_block_affluent_avg"] == 12.0
        assert result["total_water_volume_hm3"] == 300.0
        assert result["avg_flow_m3s"] == 25.0

    def test_base_dir_none(self) -> None:
        """base_dir=None is forwarded to compute_indicators."""

        @dataclass
        class FakeInd:
            """Minimal stand-in for SystemIndicators."""

            total_gen_capacity_mw: float = 0.0
            hydro_capacity_mw: float = 0.0
            thermal_capacity_mw: float = 0.0
            total_line_capacity_mw: float = 0.0
            first_block_demand_mw: float = 0.0
            last_block_demand_mw: float = 0.0
            total_energy_mwh: float = 0.0
            avg_annual_energy_mwh: float = 0.0
            first_block_affluent_avg: float = 0.0
            last_block_affluent_avg: float = 0.0
            total_water_volume_hm3: float = 0.0
            avg_flow_m3s: float = 0.0

        with patch(
            "gtopt_check_json._info.compute_indicators",
            return_value=FakeInd(),
        ) as mock_ci:
            _gtopt_indicators({})
            mock_ci.assert_called_once_with({}, base_dir=None)

    def test_returns_exactly_twelve_keys(self) -> None:
        """The returned dict has exactly the 12 expected indicator keys."""

        @dataclass
        class FakeInd:
            """Minimal stand-in for SystemIndicators."""

            total_gen_capacity_mw: float = 0.0
            hydro_capacity_mw: float = 0.0
            thermal_capacity_mw: float = 0.0
            total_line_capacity_mw: float = 0.0
            first_block_demand_mw: float = 0.0
            last_block_demand_mw: float = 0.0
            total_energy_mwh: float = 0.0
            avg_annual_energy_mwh: float = 0.0
            first_block_affluent_avg: float = 0.0
            last_block_affluent_avg: float = 0.0
            total_water_volume_hm3: float = 0.0
            avg_flow_m3s: float = 0.0

        with patch(
            "gtopt_check_json._info.compute_indicators",
            return_value=FakeInd(),
        ):
            result = _gtopt_indicators({})

        expected_keys = {
            "total_gen_capacity_mw",
            "hydro_capacity_mw",
            "thermal_capacity_mw",
            "total_line_capacity_mw",
            "first_block_demand_mw",
            "last_block_demand_mw",
            "total_energy_mwh",
            "avg_annual_energy_mwh",
            "first_block_affluent_avg",
            "last_block_affluent_avg",
            "total_water_volume_hm3",
            "avg_flow_m3s",
        }
        assert set(result.keys()) == expected_keys


# ---------------------------------------------------------------------------
# _plp_active_hydrology_indices
# ---------------------------------------------------------------------------


class TestPlpActiveHydrologyIndices:
    """Tests for _plp_active_hydrology_indices."""

    def test_returns_none_when_no_data(self) -> None:
        """Returns None when neither idsim nor aflce parser is present."""
        parser = _make_parser({})
        assert _plp_active_hydrology_indices(parser) is None

    def test_uses_aflce_when_no_idsim(self) -> None:
        """Falls back to aflce hydrology count when idsim is absent."""
        aflce_p = SimpleNamespace(
            items=[{"num_hydrologies": 4}],
        )
        parser = _make_parser({"aflce_parser": aflce_p})
        result = _plp_active_hydrology_indices(parser)
        assert result == [0, 1, 2, 3]

    def test_uses_idsim_when_present(self) -> None:
        """Uses idsim parser to extract unique hydrology indices."""
        idsim_p = MagicMock()
        idsim_p.num_simulations = 4
        # get_index(sim_idx, 1) returns 1-based hydrology indices
        # sim 0 -> hydro 3, sim 1 -> hydro 1, sim 2 -> hydro 3, sim 3 -> hydro 2
        idsim_p.get_index.side_effect = lambda sim, stage: {
            (0, 1): 3,
            (1, 1): 1,
            (2, 1): 3,
            (3, 1): 2,
        }[(sim, stage)]

        parser = _make_parser({"idsim_parser": idsim_p})
        result = _plp_active_hydrology_indices(parser)
        # Unique, preserving order: 3, 1, 2 (1-based) -> 2, 0, 1 (0-based)
        assert result == [2, 0, 1]

    def test_idsim_with_none_indices(self) -> None:
        """get_index returning None is skipped."""
        idsim_p = MagicMock()
        idsim_p.num_simulations = 3
        idsim_p.get_index.side_effect = lambda sim, stage: {
            (0, 1): 2,
            (1, 1): None,
            (2, 1): 5,
        }[(sim, stage)]

        parser = _make_parser({"idsim_parser": idsim_p})
        result = _plp_active_hydrology_indices(parser)
        assert result == [1, 4]  # 2-1=1, 5-1=4

    def test_idsim_all_none_returns_none(self) -> None:
        """Returns None when all idsim indices are None."""
        idsim_p = MagicMock()
        idsim_p.num_simulations = 2
        idsim_p.get_index.return_value = None

        parser = _make_parser({"idsim_parser": idsim_p})
        assert _plp_active_hydrology_indices(parser) is None

    def test_aflce_zero_hydrologies(self) -> None:
        """Returns None when aflce reports 0 hydrologies."""
        aflce_p = SimpleNamespace(items=[{"num_hydrologies": 0}])
        parser = _make_parser({"aflce_parser": aflce_p})
        assert _plp_active_hydrology_indices(parser) is None

    def test_aflce_empty_items(self) -> None:
        """Returns None when aflce has empty items list."""
        aflce_p = SimpleNamespace(items=[])
        parser = _make_parser({"aflce_parser": aflce_p})
        assert _plp_active_hydrology_indices(parser) is None
