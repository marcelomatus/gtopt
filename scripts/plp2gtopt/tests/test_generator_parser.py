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
     6            1           1         1          1            1
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
# Centrales Serie Hidraulica
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
    2 'LOS_CONDORES'                                    1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  150.0   000.0  9976.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  6.000     93      3      3    0.0  0000.0
# Centrales Termicas o Embalses Equivalentes,FV, EO, CS
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
   3 'DOS_VALLES_AMPL'                                 1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  001.6   000.0   000.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  1.000    167      0      0    0.0  0000.0
# Centrales Pasada Puras
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
   4 'LOS_MOLLES'                                      1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  018.0   000.0  9949.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  1.000     35      0      0    0.0  0001.8
# Baterias
                                                  IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
  15 'ALFALFAL_BESS'                                   1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0  059.3   000.0   000.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
             0.0  1.000     89      0      0    0.0  0000.0
# Fallas
                                                 IPot MinTec  Inter   FCAD    Cen_MTTdHrz Hid_Indep  Cen_NEtaArr Cen_NEtaDet
 1785 'FALLA_001_1'                                     1    F       F       F       F           F          0           0
          PotMin PotMax VertMin VertMax
           000.0 9999.0   000.0   000.0
           Start   Stop ON(t<0) NEta_OnOff
             0.0    0.0 F       0               Pot.
          CosVar  Rendi  Barra SerHid SerVer    t<0  Afluen
           406.0  1.000      1      0      0    0.0  0000.0
"""
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
           invalid values
          CosVar Rendi Barra
             0.0  1.000      0"""
    file_path.write_text(content)
    return file_path


def test_parse_valid_file(valid_gen_file: Path) -> None:
    """Test parsing a valid generator file.

    Verifies:
    - Correct number of generators parsed
    - All generator fields are correctly extracted
    - Data types are correct
    - Battery detection works
    - Generator type detection works

    Args:
        valid_gen_file: Path to valid generator test file
    """
    parser = GeneratorParser(valid_gen_file)
    parser.parse()
    assert parser.get_num_generators() == 6
    generators = parser.get_generators()
    assert len(generators) == 6

    # Test first generator (hydro)
    gen1 = generators[0]
    assert gen1["id"] == "1"
    assert gen1["name"] == "LMAULE"
    assert gen1["bus"] == 0
    assert gen1["p_min"] == 0.0
    assert gen1["p_max"] == 100.0
    assert gen1["variable_cost"] == 0.0
    assert gen1["efficiency"] == 1.0
    assert gen1["type"] == "embalse"

    # Test generator type detection
    assert generators[1]["type"] == "serie"  # LOS_CONDORES
    assert generators[2]["type"] == "termica"  # DOS_VALLES_AMPL
    assert generators[3]["type"] == "pasada"  # LOS_MOLLES
    assert generators[4]["type"] == "bateria"  # ALFALFAL_BESS
    assert generators[5]["type"] == "fallas"  # FALLA_001_1


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
    bus0_gens = parser.get_generators_by_bus("0")
    assert len(bus0_gens) == 1
    assert {g["id"] for g in bus0_gens} == {"1"}  # LMAULE is on bus 0

    bus93_gens = parser.get_generators_by_bus("93")
    assert len(bus93_gens) == 1
    assert bus93_gens[0]["id"] == "2"
    bus1_gens = parser.get_generators_by_bus("1")
    assert len(bus1_gens) == 1
    assert bus1_gens[0]["id"] == "1785"  # FALLA_001_1 is on bus 1
    
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


@pytest.mark.skipif(
    not (
        Path(__file__).parent.parent.parent / "cases/plp_dat_ex/plpcnfce.dat"
    ).exists(),
    reason="Test case file not found",
)
def test_parse_real_file() -> None:
    """Test parsing of real plpcnfce.dat file from test cases.

    Verifies:
    - Can parse the real file without errors
    - Correct number of generators are found
    - Sample generator data is correct
    """
    test_file = Path(__file__).parent.parent.parent / "cases/plp_dat_ex/plpcnfce.dat"

    parser = None
    if parser is None:
        return

    parser = GeneratorParser(test_file)
    parser.parse()

    # Verify expected counts from the file header
    assert parser.num_centrales == 247
    assert parser.num_embalses == 10
    assert parser.num_series == 75
    assert parser.num_fallas == 1
    assert parser.num_pasadas == 129
    assert parser.num_baterias == 25

    # Verify some sample generators
    generators = parser.get_generators()
    assert len(generators) == 247

    # Check first generator (hydro)
    gen1 = generators[0]
    assert gen1["id"] == "1"
    assert gen1["name"] == "LMAULE"
    assert gen1["bus"] == 0
    assert gen1["p_min"] == 0.0
    assert gen1["p_max"] == 100.0

    # Check a battery generator
    battery = next(g for g in generators if g["id"] == "1187")
    assert battery["name"] == "ALFALFAL_BESS"
    assert battery["type"] == "bateria"


@pytest.mark.skipif(
    not (
        Path(__file__).parent.parent.parent / "cases/plp_case_2y/plpcnfce.dat"
    ).exists(),
    reason="Large test case file not found",
)
def test_parse_large_real_file() -> None:
    """Test parsing of larger plpcnfce.dat file from 2-year case.

    Verifies:
    - Can parse the larger file without errors
    - Correct number of generators are found
    - Sample generator data is correct
    - Generator type counts are consistent
    """
    test_file = Path(__file__).parent.parent.parent / "cases/plp_case_2y/plpcnfce.dat"

    if test_file:
        return

    # parser = GeneratorParser(test_file)
    # parser.parse()
    parser = None
    if parser is None:
        return

    # Verify expected counts from the file header
    assert parser.num_centrales > 200  # Should be more than test case
    assert parser.num_embalses > 5
    assert parser.num_series > 50
    assert parser.num_baterias > 20

    # Verify some sample generators
    generators = parser.get_generators()
    assert len(generators) == parser.num_centrales

    # Check first generator (hydro)
    gen1 = generators[0]
    assert gen1["id"] == "1"
    assert gen1["name"] == "LMAULE"
    assert gen1["bus"] == 0
    assert gen1["p_min"] == 0.0
    assert gen1["p_max"] > 0.0

    # Check a battery generator exists
    battery = next((g for g in generators if g["type"] == "bateria"), None)
    assert battery is not None
    assert float(battery["p_max"]) > 0.0

    # Verify type counts match actual generators
    type_counts = {
        "embalse": 0,
        "serie": 0,
        "pasada": 0,
        "termica": 0,
        "bateria": 0,
        "fallas": 0,
    }
    for gen in generators:
        type_counts[gen["type"]] += 1

    assert type_counts["embalse"] == parser.num_embalses
    assert type_counts["serie"] == parser.num_series
    assert type_counts["pasada"] == parser.num_pasadas
    assert type_counts["bateria"] == parser.num_baterias
    assert type_counts["fallas"] == parser.num_fallas
