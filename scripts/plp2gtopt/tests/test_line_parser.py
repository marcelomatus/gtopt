#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for line_parser.py"""

from pathlib import Path

import pytest
from ..line_parser import LineParser


@pytest.fixture(scope="module", name="line_parser_fixture")
def line_parser_fixture():
    """Fixture that provides a parsed LineParser instance."""
    test_dir = Path(__file__).parent.parent.parent
    test_file = test_dir / "cases/plp_dat_ex/plpcnfli.dat"
    parser = LineParser(test_file)
    parser.parse()
    return parser


def test_parse_file(line_parser_fixture):
    """Test parsing the line data file."""
    assert line_parser_fixture.get_num_lines() == 10
    lines = line_parser_fixture.get_lines()
    assert len(lines) == 10


def test_line_data(line_parser_fixture):
    """Test line data structure."""
    line = line_parser_fixture.get_line_by_name("Andes220->Oeste220")
    assert line is not None
    assert line["bus_a"] == "Andes220"
    assert line["bus_b"] == "Oeste220"
    assert line["voltage"] == 220.0
    assert line["bus_a_num"] == 2
    assert line["bus_b_num"] == 65
    assert line["r"] == pytest.approx(3.431)
    assert line["x"] == pytest.approx(15.802)
    assert line["has_losses"]
    assert line["num_sections"] == 3
    assert line["is_operational"]


def test_get_lines_by_bus(line_parser_fixture):
    """Test getting lines by bus name."""
    lines = line_parser_fixture.get_lines_by_bus("Andes220")
    assert len(lines) == 2
    names = {line["name"] for line in lines}
    assert "Andes220->Oeste220" in names
    assert "Andes345->Andes220" in names


def test_get_lines_by_bus_num(line_parser_fixture):
    """Test getting lines by bus number."""
    lines = line_parser_fixture.get_lines_by_bus_num(5)
    assert len(lines) == 3
    names = {line["name"] for line in lines}
    assert "Antofag110->Desalant110" in names
    assert "Antofag110->LaNegra110" in names
    assert "Capricornio110->Antofag110" in names


def test_voltage_parsing(line_parser_fixture):
    """Test voltage extraction from line names."""
    line = line_parser_fixture.get_line_by_name("Capricorn220->Capricorn110")
    assert line["voltage"] == 220.0
    line = line_parser_fixture.get_line_by_name("Antofag110->Desalant110")
    assert line["voltage"] == 110.0


def test_operational_status(line_parser_fixture):
    """Test operational status parsing."""
    lines = line_parser_fixture.get_lines()
    for line in lines:
        assert line["is_operational"]
