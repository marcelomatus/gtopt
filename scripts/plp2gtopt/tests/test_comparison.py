"""Tests for the PLP vs gtopt element comparison formatting."""

import pytest

from plp2gtopt.plp2gtopt import (
    _cc,
    _delta_str,
    _gtopt_element_counts,
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
    assert "only bus>0 with waterway" in output

    # Check that the embalse → reservoir match note appears
    assert "embalse count" in output

    # Check that pasada → generator profiles match note appears
    assert "pasada count" in output


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
    assert "demands with bus=0 or empty excluded" in output
    assert "PLP falla (unserved energy)" in output


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
