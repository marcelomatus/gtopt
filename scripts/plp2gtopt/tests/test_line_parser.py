#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for LineParser class.

Tests include:
- Parsing valid line files
- Line data structure validation
- Bus-based line queries
- Voltage extraction from names
- Operational status parsing
- Error handling cases
"""

from pathlib import Path
import pytest

from ..line_parser import LineParser
from .conftest import get_example_file


@pytest.fixture
def sample_line_parser() -> LineParser:
    """Fixture providing initialized LineParser with sample data."""
    test_file = get_example_file("plpcnfli.dat")
    parser = LineParser(test_file)
    parser.parse()
    return parser


@pytest.fixture
def empty_file(tmp_path: Path) -> Path:
    """Fixture providing path to empty test file."""
    empty_path = tmp_path / "empty.dat"
    empty_path.touch()
    return empty_path


def test_parser_initialization():
    """Test LineParser initialization."""
    test_path = "test.dat"
    parser = LineParser(test_path)
    assert parser.file_path == Path(test_path)
    assert parser.get_lines() == []  # Use public method instead of accessing _data
    assert parser.num_lines == 0


def test_parse_sample_file(sample_line_parser):  # pylint: disable=redefined-outer-name
    """Test parsing of the sample line file."""
    parser = sample_line_parser

    # Verify basic structure
    assert parser.get_num_lines() == 10
    lines = parser.get_lines()
    assert len(lines) == 10
    assert all(isinstance(line, dict) for line in lines)


def test_line_data_structure(
    sample_line_parser,
):  # pylint: disable=redefined-outer-name
    """Test line data structure validation."""
    line = sample_line_parser.get_line_by_name("Andes220->Oeste220")
    assert line is not None

    required_fields = {
        "name": str,
        "bus_a": str,
        "bus_b": str,
        "voltage": float,
        "bus_a_num": int,
        "bus_b_num": int,
        "r": float,
        "x": float,
        "f_max_ab": float,
        "f_max_ba": float,
        "has_losses": bool,
        "num_sections": int,
        "is_operational": bool,
    }

    for field, field_type in required_fields.items():
        assert field in line
        assert isinstance(line[field], field_type), f"Field {field} has wrong type"


def test_line_name_parsing():
    """Test line name parsing logic."""
    test_cases = [
        ("Bus1->Bus2", ("Bus1", "Bus2")),
        ("BusA-BusB", ("BusA", "BusB")),
        ("LongName->Short", ("LongName", "Short")),
    ]

    for name, expected in test_cases:
        parser = LineParser("test.dat")
        result = parser.parse_line_name(name)
        assert result == expected


def test_invalid_line_names():
    """Test handling of invalid line names."""
    invalid_names = ["Bus1", "Bus1>Bus2", "Bus1-Bus2-Bus3", ""]

    parser = LineParser("test.dat")
    for name in invalid_names:
        with pytest.raises(ValueError):
            parser.parse_line_name(name)  # Use public method


def test_get_lines_by_bus(sample_line_parser):  # pylint: disable=redefined-outer-name
    """Test getting lines filtered by bus name."""
    # Test existing bus
    lines = sample_line_parser.get_lines_by_bus("Andes220")
    assert len(lines) == 2
    names = {line["name"] for line in lines}
    assert "Andes220->Oeste220" in names
    assert "Andes345->Andes220" in names

    # Test non-existent bus
    assert len(sample_line_parser.get_lines_by_bus("NonExistentBus")) == 0


def test_get_lines_by_bus_num(
    sample_line_parser,
):  # pylint: disable=redefined-outer-name
    """Test getting lines filtered by bus number."""
    # Test existing bus number
    lines = sample_line_parser.get_lines_by_bus_num(5)
    assert len(lines) == 3
    names = {line["name"] for line in lines}
    assert "Antofag110->Desalant110" in names
    assert "Antofag110->LaNegra110" in names
    assert "Capricornio110->Antofag110" in names

    # Test non-existent bus number
    assert len(sample_line_parser.get_lines_by_bus_num(999)) == 0


def test_voltage_extraction(sample_line_parser):  # pylint: disable=redefined-outer-name
    """Test voltage extraction from line names."""
    # Test with actual lines from sample data
    test_cases = [
        ("Andes220->Oeste220", 220.0),
        ("Antofag110->Desalant110", 110.0),
        ("Andes345->Andes220", 345.0),
    ]

    for name, expected_voltage in test_cases:
        line = sample_line_parser.get_line_by_name(name)
        if line is None:
            pytest.skip(f"Test line '{name}' not found in sample data")
        assert line["voltage"] == expected_voltage


def test_parse_empty_file(empty_file):  # pylint: disable=redefined-outer-name
    """Test handling of empty input file."""
    parser = LineParser(empty_file)
    with pytest.raises(ValueError, match="File is empty"):
        parser.parse()


def test_parse_nonexistent_file():
    """Test handling of non-existent input file."""
    parser = LineParser("nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_operational_status(sample_line_parser):  # pylint: disable=redefined-outer-name
    """Test operational status parsing."""
    lines = sample_line_parser.get_lines()
    for line in lines:
        assert "is_operational" in line
        assert isinstance(line["is_operational"], bool)
