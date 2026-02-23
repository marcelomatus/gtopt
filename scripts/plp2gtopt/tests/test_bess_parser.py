"""Unit tests for BessParser class."""

from pathlib import Path
import pytest

from ..bess_parser import BessParser


def test_bess_parser_initialization():
    """Test BessParser initialization."""
    parser = BessParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.besses
    assert parser.num_besses == 0


def test_parse_zero_besses(tmp_path):
    """Test parsing a file with 0 BESS entries."""
    f = tmp_path / "plpbess.dat"
    f.write_text("# Numero de BESS\n 0\n")
    parser = BessParser(f)
    parser.parse()
    assert parser.num_besses == 0
    assert not parser.besses


def test_parse_single_bess(tmp_path):
    """Test parsing a single BESS entry."""
    f = tmp_path / "plpbess.dat"
    f.write_text(
        "# Numero de BESS\n"
        " 1\n"
        "# Num  Nombre  Barra  PMaxC  PMaxD  nc  nd  HrsReg  EtaIni  EtaFin  VolIni  NCiclos\n"
        "    1  'BESS1'     1   50.0   50.0  0.95  0.95  4.0    1    10    0.50    1.0\n"
    )
    parser = BessParser(f)
    parser.parse()

    assert parser.num_besses == 1
    b = parser.besses[0]
    assert b["number"] == 1
    assert b["name"] == "BESS1"
    assert b["bus"] == 1
    assert b["pmax_charge"] == pytest.approx(50.0)
    assert b["pmax_discharge"] == pytest.approx(50.0)
    assert b["nc"] == pytest.approx(0.95)
    assert b["nd"] == pytest.approx(0.95)
    assert b["hrs_reg"] == pytest.approx(4.0)
    assert b["eta_ini"] == 1
    assert b["eta_fin"] == 10
    assert b["vol_ini"] == pytest.approx(0.50)
    assert b["n_ciclos"] == pytest.approx(1.0)


def test_parse_multiple_besses(tmp_path):
    """Test parsing multiple BESS entries."""
    f = tmp_path / "plpbess.dat"
    f.write_text(
        " 2\n"
        "    1  'BESS1'     1   100.0  100.0  0.95  0.95  4.0    1    10    0.50    1.0\n"
        "    2  'BESS2'     2    50.0   50.0  0.90  0.90  2.0    3     8    0.50    1.0\n"
    )
    parser = BessParser(f)
    parser.parse()

    assert parser.num_besses == 2
    assert parser.besses[0]["name"] == "BESS1"
    assert parser.besses[1]["name"] == "BESS2"
    assert parser.besses[1]["bus"] == 2
    assert parser.besses[1]["pmax_charge"] == pytest.approx(50.0)
    assert parser.besses[1]["nc"] == pytest.approx(0.90)
    assert parser.besses[1]["eta_ini"] == 3
    assert parser.besses[1]["eta_fin"] == 8


def test_parse_with_comments(tmp_path):
    """Test that comment lines are skipped."""
    f = tmp_path / "plpbess.dat"
    f.write_text(
        "# Header\n"
        " 1\n"
        "# Row comment\n"
        "    1  'MyBESS'    1   20.0   20.0  0.98  0.98  8.0    1     5    0.30    2.0\n"
    )
    parser = BessParser(f)
    parser.parse()
    assert parser.num_besses == 1
    assert parser.besses[0]["name"] == "MyBESS"


def test_get_bess_by_name(tmp_path):
    """Test lookup by name."""
    f = tmp_path / "plpbess.dat"
    f.write_text(
        " 1\n"
        "    1  'Alpha'     1   50.0   50.0  0.95  0.95  4.0    1    10    0.50    1.0\n"
    )
    parser = BessParser(f)
    parser.parse()
    bess_alpha = parser.get_bess_by_name("Alpha")
    assert bess_alpha is not None
    assert bess_alpha["number"] == 1
    assert parser.get_bess_by_name("NoSuch") is None


def test_get_bess_by_number(tmp_path):
    """Test lookup by number."""
    f = tmp_path / "plpbess.dat"
    f.write_text(
        " 1\n"
        "    7  'B7'        3   10.0   10.0  0.90  0.90  2.0    1     2    0.50    1.0\n"
    )
    parser = BessParser(f)
    parser.parse()
    bess_b7 = parser.get_bess_by_number(7)
    assert bess_b7 is not None
    assert bess_b7["name"] == "B7"
    assert parser.get_bess_by_number(99) is None


def test_parse_empty_file_raises(tmp_path):
    """Test that an empty file raises ValueError."""
    f = tmp_path / "plpbess.dat"
    f.touch()
    parser = BessParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_missing_file_raises():
    """Test that a missing file raises FileNotFoundError on parse."""
    parser = BessParser("/nonexistent/plpbess.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()
