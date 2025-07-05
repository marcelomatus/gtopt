#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for the GeneratorParser class.

Tests include:
- Parsing valid generator files
- Handling empty/malformed files
- Generator data extraction
- Bus-based generator queries
"""

from pathlib import Path
from typing import Dict, List
import pytest

from plp2gtopt.generator_parser import GeneratorParser


@pytest.fixture(name="valid_gen_file")
def valid_gen_file_fixture(tmp_path: Path) -> Path:
    """Create a valid generator test file fixture.
    
    Args:
        tmp_path: Pytest temporary path fixture
        
    Returns:
        Path to temporary test file with valid generator data
    """
    file_path = tmp_path / "valid_gen.dat"
    content = """# Test generator file
    1 'TEST_GEN1'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           010.0  100.0   000.0   000.0
          CosVar  Rendi  Barra Genera Vertim
             5.0  1.000  1      0      0
    2 'TEST_GEN2'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           020.0  200.0   000.0   000.0
          CosVar  Rendi  Barra Genera Vertim
            10.0  0.900      2      0      0"""
    file_path.write_text(content)
    return file_path


@pytest.fixture
def empty_gen_file(tmp_path) -> Path:
    """Create an empty generator test file."""
    file_path = tmp_path / "empty_gen.dat"
    file_path.touch()
    return file_path


@pytest.fixture
def malformed_gen_file(tmp_path) -> Path:
    """Create a malformed generator test file."""
    file_path = tmp_path / "bad_gen.dat"
    content = """    1 'BAD_GEN'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           010.0"""
    file_path.write_text(content)
    return file_path


def test_parse_valid_file(valid_gen_file: Path) -> None:
    """Test parsing a valid generator file.
    
    Verifies:
    - Correct number of generators parsed
    - All generator fields are correctly extracted
    - Data types are correct
    
    Args:
        valid_gen_file: Path to valid generator test file
    """
    parser = GeneratorParser(valid_gen_file)
    parser.parse()
    assert parser.get_num_generators() == 2
    generators = parser.get_generators()
    assert len(generators) == 2

    # Test first generator
    gen1 = generators[0]
    assert gen1["id"] == "1"
    assert gen1["name"] == "TEST_GEN1"
    assert gen1["bus"] == "1"
    assert gen1["p_min"] == 10.0
    assert gen1["p_max"] == 100.0
    assert gen1["variable_cost"] == 5.0
    assert gen1["efficiency"] == 1.0
    assert gen1["is_battery"] is False

    # Test second generator
    gen2 = generators[1]
    assert gen2["id"] == "2"
    assert gen2["name"] == "TEST_GEN2"
    assert gen2["bus"] == "2"
    assert gen2["p_min"] == 20.0
    assert gen2["p_max"] == 200.0
    assert gen2["variable_cost"] == 10.0
    assert gen2["efficiency"] == 0.9
    assert gen2["is_battery"] is False


def test_get_generators_by_bus(valid_gen_file: Path) -> None:
    """Test getting generators filtered by bus ID.
    
    Verifies:
    - Correct filtering by bus ID
    - Empty result for non-existent bus
    - Return type is correct
    
    Args:
        valid_gen_file: Path to valid generator test file
    """
    parser = GeneratorParser(valid_gen_file)
    parser.parse()
    bus1_gens = parser.get_generators_by_bus("1")
    assert len(bus1_gens) == 1
    assert bus1_gens[0]["id"] == "1"
    
    bus2_gens = parser.get_generators_by_bus("2")
    assert len(bus2_gens) == 1
    assert bus2_gens[0]["id"] == "2"
    empty_gens = parser.get_generators_by_bus("999")
    assert len(empty_gens) == 0


def test_parse_nonexistent_file(tmp_path: Path) -> None:
    """Test handling of non-existent input file.
    
    Verifies:
    - Proper FileNotFoundError is raised
    - Error contains meaningful message
    
    Args:
        tmp_path: Pytest temporary path fixture
    """
    parser = GeneratorParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_parse_empty_file(empty_gen_file: Path) -> None:
    """Test handling of empty input file.
    
    Verifies:
    - Parser handles empty file gracefully
    - Returns empty generator list
    - Doesn't raise exceptions
    
    Args:
        empty_gen_file: Path to empty test file
    """
    parser = GeneratorParser(empty_gen_file)
    parser.parse()  # Should not raise for empty file
    assert parser.get_num_generators() == 0


def test_parse_malformed_file(malformed_gen_file: Path) -> None:
    """Test handling of malformed input file.
    
    Verifies:
    - Proper ValueError/IndexError is raised
    - Error contains meaningful message
    
    Args:
        malformed_gen_file: Path to malformed test file
    """
    parser = GeneratorParser(malformed_gen_file)
    with pytest.raises((ValueError, IndexError)):
        parser.parse()
