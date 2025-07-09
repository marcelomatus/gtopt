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
    assert not parser.get_lines()  # Use public method instead of accessing _data
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
        "number": int,
        "name": str,
        "active": bool,
        "bus_a": int,
        "bus_b": int,
        "voltage": float,
        "r": float,
        "x": float,
        "fmax_ab": float,
        "fmax_ba": float,
        "mod_perdidas": bool,
        "num_sections": int,
    }

    for field, field_type in required_fields.items():
        assert field in line
        assert isinstance(line[field], field_type), f"Field {field} has wrong type"


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
        assert "active" in line
        assert isinstance(line["active"], bool)
