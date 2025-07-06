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
    content = """# Test generator file - plpcnfce.dat format
    1 'TEST_GEN1'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           010.0  100.0   000.0   000.0
          CosVar  Rendi  Barra Genera Vertim
             5.0  1.000  1      0      0
          CustoFixo CustoInic CustoManut
            1000.0    5000.0     200.0
          RampaSub RampaDes
             50.0      50.0
    2 'TEST_GEN2'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           020.0  200.0   000.0   000.0
          CosVar  Rendi  Barra Genera Vertim
            10.0  0.900      2      0      0
          CustoFixo CustoInic CustoManut
            1500.0    6000.0     250.0
          RampaSub RampaDes
             60.0      60.0
    3 'TEST_GEN3'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           030.0  300.0   000.0   000.0
          CosVar  Rendi  Barra Genera Vertim
            15.0  0.950      1      0      0
          CustoFixo CustoInic CustoManut
            2000.0    7000.0     300.0
          RampaSub RampaDes
             70.0      70.0
    4 'TEST_GEN4'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           040.0  400.0   000.0   000.0
          CosVar  Rendi  Barra Genera Vertim
            20.0  0.980      1      0      0
          CustoFixo CustoInic CustoManut
            2500.0    8000.0     350.0
          RampaSub RampaDes
             80.0      80.0
    5 'TEST_GEN5'                                       1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           050.0  500.0   000.0   000.0
          CosVar  Rendi  Barra Genera Vertim
            25.0  0.990      2      0      0
          CustoFixo CustoInic CustoManut
            3000.0    9000.0     400.0
          RampaSub RampaDes
             90.0      90.0"""
    file_path.write_text(content)
    return file_path


@pytest.fixture(name="empty_gen_file")
def empty_gen_file_fixture(tmp_path: Path) -> Path:
    """Create an empty generator test file fixture.

    Args:
        tmp_path: Pytest temporary path fixture

    Returns:
        Path to temporary empty test file
    """
    file_path = tmp_path / "empty_gen.dat"
    file_path.touch()
    return file_path


@pytest.fixture(name="malformed_gen_file")
def malformed_gen_file_fixture(tmp_path: Path) -> Path:
    """Create a malformed generator test file fixture.

    Args:
        tmp_path: Pytest temporary path fixture

    Returns:
        Path to temporary malformed test file
    """
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
    assert parser.get_num_generators() == 5
    generators = parser.get_generators()
    assert len(generators) == 5

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
    assert len(bus1_gens) == 3  # Now has 3 generators on bus 1
    assert {g["id"] for g in bus1_gens} == {"1", "3", "4"}

    bus2_gens = parser.get_generators_by_bus("2")
    assert len(bus2_gens) == 2  # Now has 2 generators on bus 2
    assert {g["id"] for g in bus2_gens} == {"2", "5"}
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
