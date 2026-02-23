"""Integration tests for plp2gtopt using the plp_dat_ex sample case."""

import json
from pathlib import Path

import pytest

from plp2gtopt.plp_parser import PLPParser

# Path to the sample PLP case shipped with the repository
_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPDatEx = _CASES_DIR / "plp_dat_ex"


@pytest.mark.integration
def test_plp_parser_parses_dat_ex(tmp_path):
    """Integration test: PLPParser can parse all files in plp_dat_ex."""
    parser = PLPParser({"input_dir": _PLPDatEx})
    parser.parse_all()

    # All 12 parsers must have run
    expected_keys = [
        "block_parser",
        "stage_parser",
        "bus_parser",
        "line_parser",
        "central_parser",
        "demand_parser",
        "cost_parser",
        "mance_parser",
        "manli_parser",
        "aflce_parser",
        "extrac_parser",
        "manem_parser",
    ]
    for key in expected_keys:
        assert key in parser.parsed_data, f"Missing parser result: {key}"


@pytest.mark.integration
def test_plp_dat_ex_blocks_and_stages():
    """Integration test: plp_dat_ex has valid block and stage data."""
    parser = PLPParser({"input_dir": _PLPDatEx})
    parser.parse_all()

    blocks = parser.parsed_data["block_parser"].blocks
    stages = parser.parsed_data["stage_parser"].stages

    assert len(blocks) > 0, "No blocks parsed"
    assert len(stages) > 0, "No stages parsed"

    # Every block must have required fields with valid values
    for block in blocks:
        assert block["number"] > 0
        assert block["stage"] > 0
        assert block["duration"] > 0


@pytest.mark.integration
def test_plp_dat_ex_buses():
    """Integration test: plp_dat_ex has valid bus data."""
    parser = PLPParser({"input_dir": _PLPDatEx})
    parser.parse_all()

    buses = parser.parsed_data["bus_parser"]
    bus_list = buses.buses

    assert len(bus_list) > 0, "No buses parsed"
    for bus in bus_list:
        assert "name" in bus
        assert "number" in bus
        assert bus["number"] > 0

