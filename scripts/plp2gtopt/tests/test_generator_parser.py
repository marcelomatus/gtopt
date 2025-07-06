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
    content = """# Archivo de configuracion de las centrales (plpcnfce.dat)
# Num.Centrales  Num.Embalses Num.Serie Num.Fallas Num.Pas.Pur. Num.BAT
     5            2           1       0        0         1
# Interm Min.Tec. Cos.Arr.Det. FFaseSinMT EtapaCambioFase
  F      F        F            F          00
# Caracteristicas Centrales
# Centrales de Embalse
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
    1 'LMAULE'                                          1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  100.0   000.0   000.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.           Volumen    Volumen    Volumen    Volumen  Factor
          CosVar  Rendi  Barra Genera Vertim    t<0  Afluen    Inicial      Final     Minimo     Maximo  Escala EmbCFUE
             0.0  1.000      0      2      0    0.0  0012.0  0.6570569  1.2934600  0.0000000  1.4534093  1.0E+9       T
    2 'LOS_CONDORES'                                    1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  150.0   000.0  9976.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  6.000     93      3      3    0.0  0000.0
# Centrales Serie Hidraulica
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
    3 'B_LaMina'                                        1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  007.0   000.0  9975.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  1.000      0      5      4    0.0  0020.8
# Centrales Termicas o Embalses Equivalentes,FV, EO, CS
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
  171 'DOS_VALLES_AMPL'                                 1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  001.6   000.0   000.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  1.000    167      0      0    0.0  0000.0
# Baterias
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
 1187 'ALFALFAL_BESS'                                   1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  059.3   000.0   000.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  1.000     89      0      0    0.0  0000.0"""
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
    - Battery detection works

    Args:
        valid_gen_file: Path to valid generator test file
    """
    parser = GeneratorParser(valid_gen_file)
    parser.parse()
    assert parser.get_num_generators() == 5
    generators = parser.get_generators()
    assert len(generators) == 5

    # Test first generator (hydro)
    gen1 = generators[0]
    assert gen1["id"] == "1"
    assert gen1["name"] == "LMAULE"
    assert gen1["bus"] == "0"
    assert gen1["p_min"] == 0.0
    assert gen1["p_max"] == 100.0
    assert gen1["variable_cost"] == 0.0
    assert gen1["efficiency"] == 1.0
    assert gen1["is_battery"] is False
    assert gen1["type"] == "embalse"

    # Test battery generator
    bat_gen = next(g for g in generators if g["is_battery"])
    assert bat_gen["id"] == "1187"
    assert "BESS" in bat_gen["name"]
    assert bat_gen["type"] == "bateria"

    # Test generator type determination
    with pytest.raises(ValueError):
        parser._determine_generator_type(999)  # Invalid index


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
    - Parser properly detects empty file
    - Raises ValueError with appropriate message

    Args:
        empty_gen_file: Path to empty test file
    """
    parser = GeneratorParser(empty_gen_file)
    with pytest.raises(ValueError, match="File is empty"):
        parser.parse()


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
"""Unit tests for GeneratorParser class."""

import pytest
from pathlib import Path
from ..generator_parser import GeneratorParser

@pytest.fixture
def valid_generator_file(tmp_path):
    file = tmp_path / "valid_gen.dat"
    content = """3 2 0 0 0 1  # 2 hydro, 0 series, 1 battery
1 'GEN1'
PotMin PotMax VMin VMax
10 100 0 1
CosVar Rendi Barra
5.0 0.9 101

2 'GEN2'
PotMin PotMax VMin VMax
20 200 0 1
CosVar Rendi Barra
6.0 0.85 102

3 'BATTERY1'
PotMin PotMax VMin VMax
-50 50 0 1
CosVar Rendi Barra
4.5 0.95 101"""
    file.write_text(content)
    return file

@pytest.fixture
def empty_file(tmp_path):
    file = tmp_path / "empty.dat"
    file.touch()
    return file

@pytest.fixture
def malformed_file(tmp_path):
    file = tmp_path / "malformed.dat"
    content = """1 'BAD_GEN'
PotMin PotMax VMin VMax
invalid values"""
    file.write_text(content)
    return file

def test_parse_valid_file(valid_generator_file):
    """Test parsing a valid generator file."""
    parser = GeneratorParser(valid_generator_file)
    parser.parse()
    
    assert parser.num_generators == 3
    generators = parser.get_generators()
    
    gen1 = generators[0]
    assert gen1["id"] == "1"
    assert gen1["name"] == "GEN1"
    assert gen1["bus"] == "101"
    assert gen1["p_min"] == 10.0
    assert gen1["p_max"] == 100.0
    assert gen1["variable_cost"] == 5.0
    assert gen1["efficiency"] == 0.9
    assert gen1["is_battery"] is False

def test_battery_detection(valid_generator_file):
    """Test battery storage detection."""
    parser = GeneratorParser(valid_generator_file)
    parser.parse()
    
    generators = parser.get_generators()
    assert generators[2]["is_battery"] is True

def test_get_generators_by_bus(valid_generator_file):
    """Test filtering generators by bus."""
    parser = GeneratorParser(valid_generator_file)
    parser.parse()
    
    bus101_gens = parser.get_generators_by_bus("101")
    assert len(bus101_gens) == 2
    assert all(g["bus"] == "101" for g in bus101_gens)

def test_invalid_file(tmp_path):
    """Test handling of invalid files."""
    non_existent_file = tmp_path / "does_not_exist.dat"
    with pytest.raises(FileNotFoundError):
        GeneratorParser(non_existent_file).parse()

def test_empty_file(empty_file):
    """Test handling of empty file."""
    parser = GeneratorParser(empty_file)
    with pytest.raises(ValueError, match="File is empty"):
        parser.parse()

def test_malformed_file(malformed_file):
    """Test handling of malformed file."""
    parser = GeneratorParser(malformed_file)
    with pytest.raises(ValueError):
        parser.parse()

def test_generator_type_detection(valid_generator_file):
    """Test generator type classification."""
    parser = GeneratorParser(valid_generator_file)
    parser.parse()
    
    generators = parser.get_generators()
    # First generator in test file is hydro (embalse)
    assert generators[0]["type"] == "embalse"
    # Third generator is battery
    assert generators[2]["type"] == "bateria"


def test_real_plpcnfce_file():
    """Test parsing of real plpcnfce.dat example file."""
    test_file = Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / "plpcnfce.dat"
    parser = GeneratorParser(test_file)
    parser.parse()
    
    # Verify counts match header
    assert parser.num_centrales == 247
    assert parser.num_embalses == 10
    assert parser.num_series == 75
    assert parser.num_pasadas == 129
    assert parser.num_baterias == 25
    assert parser.num_fallas == 1
    
    generators = parser.get_generators()
    assert len(generators) == 247
    
    # Spot check some known generators
    lmaule = next(g for g in generators if g["name"] == "LMAULE")
    assert lmaule["type"] == "embalse"
    assert lmaule["bus"] == "0"
    assert lmaule["p_max"] == 100.0
    
    alfalfal_bess = next(g for g in generators if g["name"] == "ALFALFAL_BESS")
    assert alfalfal_bess["type"] == "bateria"
    assert alfalfal_bess["is_battery"] is True
    assert alfalfal_bess["p_max"] == 59.3
    
    # Verify all required fields exist for each generator
    required_fields = {"id", "name", "bus", "p_min", "p_max", "variable_cost", "efficiency", "type"}
    for gen in generators:
        assert all(field in gen for field in required_fields)
