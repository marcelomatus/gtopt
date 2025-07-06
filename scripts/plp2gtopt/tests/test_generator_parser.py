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
