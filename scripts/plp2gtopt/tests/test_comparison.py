"""Tests for the PLP vs gtopt element comparison formatting."""

import logging

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
            "generator_array": [{"uid": 1}],
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
    assert counts["generators"] == 1
    assert counts["demands"] == 3
    assert counts["batteries"] == 1
    assert counts["turbines"] == 2
    assert counts["blocks"] == 24
    assert counts["scenarios"] == 3


# ---------------------------------------------------------------------------
# _log_comparison — integration test via log capture
# ---------------------------------------------------------------------------


def test_log_comparison_output(caplog: pytest.LogCaptureFixture) -> None:
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

    with caplog.at_level(logging.INFO, logger="plp2gtopt.plp2gtopt"):
        _log_comparison(plp, gtopt)

    output = caplog.text

    # Section headers present
    assert "Network" in output
    assert "Generation" in output
    assert "Hydro System" in output
    assert "Storage" in output
    assert "Loads" in output
    assert "Simulation" in output

    # Key comparison header
    assert "PLP" in output
    assert "gtopt" in output

    # Derived rows
    assert "gen (excl falla+bat)" in output
    assert "hydro centrals (emb+ser)" in output

    # Analysis notes
    assert "→ turbines + generators" in output
    assert "→ generators + profiles" in output
    assert "→ batteries" in output
    assert "excluded from gtopt" in output
    assert "only bus>0 with waterway" in output

    # Check that the embalse → reservoir match note appears
    assert "= embalse count" in output

    # Check that pasada → generator profiles match note appears
    assert "= pasada count" in output


def test_log_comparison_gen_delta(caplog: pytest.LogCaptureFixture) -> None:
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

    with caplog.at_level(logging.INFO, logger="plp2gtopt.plp2gtopt"):
        _log_comparison(plp, gtopt)

    output = caplog.text
    assert "centrals with bus<=0" in output
