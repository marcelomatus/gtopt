#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for the CentralParser class.

Tests include:
- Parsing valid central files
- Handling empty/malformed files
- Central data extraction
- Bus-based central queries
"""

from typing import Dict, Any
from pathlib import Path
import pytest

from plp2gtopt.central_parser import CentralParser


@pytest.fixture(name="valid_gen_file")
def valid_gen_file_fixture(tmp_path: Path) -> Path:
    """Create a valid central test file fixture.

    Args:
        tmp_path: Pytest temporary path fixture

    Returns:
        Path to temporary test file with valid central data
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
    """Create an empty central test file fixture.

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
    """Create a malformed central test file fixture.

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
    """Test parsing a valid central file."""
    parser = CentralParser(valid_gen_file)
    parser.parse()
    assert parser.num_centrals == 6
    centrals = parser.centrals
    assert len(centrals) == 6

    # Test first central (hydro)
    gen1 = centrals[0]
    assert gen1["number"] == 1
    assert gen1["name"] == "LMAULE"
    assert gen1["bus"] == 0
    assert gen1["pmin"] == 0.0
    assert gen1["pmax"] == 100.0
    assert gen1["gcost"] == 0.0
    assert gen1["efficiency"] == 1.0
    assert gen1["type"] == "embalse"

    # Test central type detection
    assert centrals[1]["type"] == "serie"  # LOS_CONDORES
    assert centrals[2]["type"] == "termica"  # DOS_VALLES_AMPL
    assert centrals[3]["type"] == "pasada"  # LOS_MOLLES
    assert centrals[4]["type"] == "bateria"  # ALFALFAL_BESS
    assert centrals[5]["type"] == "falla"  # FALLA_001_1


def test_parse_nonexistent_file(tmp_path: Path) -> None:
    """Test handling of non-existent input file."""
    parser = CentralParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_parse_empty_file(empty_gen_file: Path) -> None:
    """Test handling of empty input file."""
    parser = CentralParser(empty_gen_file)
    with pytest.raises(ValueError, match="File is empty"):
        parser.parse()


@pytest.mark.skipif(
    not (
        Path(__file__).parent.parent.parent / "cases/plp_dat_ex/plpcnfce.dat"
    ).exists(),
    reason="Test case file not found",
)
def test_parse_real_file() -> None:
    """Test parsing of real plpcnfce.dat file from test cases."""
    test_file = Path(__file__).parent.parent.parent / "cases/plp_dat_ex/plpcnfce.dat"

    parser = CentralParser(test_file)
    parser.parse()

    # Verify expected counts from the file header
    assert parser.num_centrales == 247
    assert parser.num_embalses == 10
    assert parser.num_series == 75
    assert parser.num_fallas == 1
    assert parser.num_pasadas == 129
    assert parser.num_baterias == 25

    # Verify some sample centrals
    centrals = parser.centrals
    assert len(centrals) == 247

    # Check first central (hydro)
    gen1 = centrals[0]
    assert gen1["number"] == 1
    assert gen1["name"] == "LMAULE"
    assert gen1["bus"] == 0
    assert gen1["pmin"] == 0.0
    assert gen1["pmax"] == 100.0

    # Check a battery central
    battery = next(g for g in centrals if g["number"] == 1187)
    assert battery["name"] == "ALFALFAL_BESS"
    assert battery["type"] == "bateria"

    # Verify type counts match actual centrals
    type_counts: Dict[str, int] = {
        "embalse": 0,
        "serie": 0,
        "pasada": 0,
        "termica": 0,
        "bateria": 0,
        "falla": 0,
    }

    for gen in centrals:
        type_counts[str(gen["type"])] += 1

    assert type_counts["embalse"] == parser.num_embalses
    assert type_counts["serie"] == parser.num_series
    assert type_counts["pasada"] == parser.num_pasadas
    assert type_counts["bateria"] == parser.num_baterias
    assert type_counts["falla"] == parser.num_fallas


@pytest.mark.skipif(
    not (
        Path(__file__).parent.parent.parent / "cases/plp_case_2y/plpcnfce.dat"
    ).exists(),
    reason="Large test case file not found",
)
def test_parse_large_real_file() -> None:
    """Test parsing of larger plpcnfce.dat file from 2-year case."""
    test_file = Path(__file__).parent.parent.parent / "cases/plp_case_2y/plpcnfce.dat"

    parser = CentralParser(test_file)
    parser.parse()

    # Verify expected counts from the file header
    assert parser.num_centrales == 2732  # Should be more than test case
    assert parser.num_embalses == 10
    assert parser.num_series == 75
    assert parser.num_pasadas == 129
    assert parser.num_baterias == 25
    assert parser.num_termicas == 1545
    assert parser.num_fallas == 948

    # Verify some sample centrals
    centrals = parser.centrals
    assert len(centrals) == parser.num_centrales

    # Check first central (hydro)
    gen1 = centrals[0]
    assert gen1["number"] == 1
    assert gen1["name"] == "LMAULE"
    assert gen1["bus"] == 0
    assert gen1["pmin"] == 0.0
    assert float(gen1["pmax"]) > 0.0

    # Check a battery central exists
    battery = next((g for g in centrals if g["type"] == "bateria"), None)
    assert battery is not None
    assert float(battery["pmax"]) > 0.0

    # Verify type counts match actual centrals
    type_counts: Dict[str, Any] = {
        "embalse": 0,
        "serie": 0,
        "pasada": 0,
        "termica": 0,
        "bateria": 0,
        "falla": 0,
    }

    for gen in centrals:
        type_counts[str(gen["type"])] += 1

    assert type_counts["embalse"] == parser.num_embalses
    assert type_counts["serie"] == parser.num_series
    assert type_counts["pasada"] == parser.num_pasadas
    assert type_counts["bateria"] == parser.num_baterias
    assert type_counts["falla"] == parser.num_fallas


def test_parse_malformed_file(malformed_gen_file: Path) -> None:
    """Test handling of malformed central file."""
    parser = CentralParser(malformed_gen_file)
    with pytest.raises(ValueError):
        parser.parse()


def test_central_type_detection() -> None:
    """Test central type detection logic."""
    parser = CentralParser("dummy.dat")

    # Test type detection with mock counts
    parser.num_embalses = 2
    parser.num_series = 3
    parser.num_termicas = 5
    parser.num_pasadas = 1
    parser.num_baterias = 2
    parser.num_fallas = 1

    # Verify type detection for each range using test helper method
    assert parser.get_central_type(0) == "embalse"
    assert parser.get_central_type(1) == "embalse"
    assert parser.get_central_type(2) == "serie"
    assert parser.get_central_type(3) == "serie"
    assert parser.get_central_type(4) == "serie"
    assert parser.get_central_type(5) == "termica"
    assert parser.get_central_type(6) == "termica"
    assert parser.get_central_type(7) == "termica"
    assert parser.get_central_type(8) == "termica"
    assert parser.get_central_type(9) == "termica"
    assert parser.get_central_type(10) == "pasada"
    assert parser.get_central_type(11) == "bateria"
    assert parser.get_central_type(12) == "bateria"
    assert parser.get_central_type(13) == "falla"
    assert parser.get_central_type(14) == "unknown"


def test_get_central_by_name(valid_gen_file: Path) -> None:
    """Test lookup of central by name."""
    parser = CentralParser(valid_gen_file)
    parser.parse()

    central = parser.get_central_by_name("LMAULE")
    assert central is not None
    assert central["name"] == "LMAULE"

    # Test non-existent central
    assert parser.get_central_by_name("NON_EXISTENT") is None


def test_central_properties(valid_gen_file: Path) -> None:
    """Test central properties access."""
    parser = CentralParser(valid_gen_file)
    parser.parse()

    assert len(parser.centrals) == parser.num_centrals
    assert isinstance(parser.centrals, list)
    assert all(isinstance(c, dict) for c in parser.centrals)
