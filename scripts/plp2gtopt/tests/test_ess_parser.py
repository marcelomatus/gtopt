"""Unit tests for EssParser class."""

from pathlib import Path
import pytest

from ..ess_parser import EssParser


def test_ess_parser_initialization():
    """Test EssParser initialization."""
    parser = EssParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.esses
    assert parser.num_esses == 0


def test_parse_zero_esses(tmp_path):
    """Test parsing a file with 0 ESS entries."""
    f = tmp_path / "plpess.dat"
    f.write_text("# Numero de ESS\n 0\n")
    parser = EssParser(f)
    parser.parse()
    assert parser.num_esses == 0
    assert parser.esses == []


def test_parse_single_ess(tmp_path):
    """Test parsing a single ESS entry."""
    f = tmp_path / "plpess.dat"
    f.write_text(
        "# Numero de ESS\n"
        " 1\n"
        "# Num  Nombre  Barra  PMaxC  PMaxD  nc  nd  HrsReg  VolIni\n"
        "    1  'ESS1'      1   100.0  100.0  0.95  0.95   4.0   0.50\n"
    )
    parser = EssParser(f)
    parser.parse()

    assert parser.num_esses == 1
    e = parser.esses[0]
    assert e["number"] == 1
    assert e["name"] == "ESS1"
    assert e["bus"] == 1
    assert e["pmax_charge"] == pytest.approx(100.0)
    assert e["pmax_discharge"] == pytest.approx(100.0)
    assert e["nc"] == pytest.approx(0.95)
    assert e["nd"] == pytest.approx(0.95)
    assert e["hrs_reg"] == pytest.approx(4.0)
    assert e["vol_ini"] == pytest.approx(0.50)
    # ESS has no eta_ini / eta_fin / n_ciclos
    assert "eta_ini" not in e
    assert "eta_fin" not in e
    assert "n_ciclos" not in e


def test_parse_multiple_esses(tmp_path):
    """Test parsing multiple ESS entries."""
    f = tmp_path / "plpess.dat"
    f.write_text(
        " 2\n"
        "    1  'ESS1'      1   100.0  100.0  0.95  0.95   4.0   0.50\n"
        "    2  'ESS2'      2    80.0   80.0  0.90  0.90   2.0   0.30\n"
    )
    parser = EssParser(f)
    parser.parse()

    assert parser.num_esses == 2
    assert parser.esses[0]["name"] == "ESS1"
    assert parser.esses[1]["name"] == "ESS2"
    assert parser.esses[1]["bus"] == 2
    assert parser.esses[1]["nc"] == pytest.approx(0.90)


def test_parse_with_comments(tmp_path):
    """Test that comment lines are skipped."""
    f = tmp_path / "plpess.dat"
    f.write_text(
        "# Header\n"
        " 1\n"
        "# Row\n"
        "    1  'MyESS'     1    50.0   50.0  0.98  0.98   8.0   0.40\n"
    )
    parser = EssParser(f)
    parser.parse()
    assert parser.num_esses == 1
    assert parser.esses[0]["name"] == "MyESS"


def test_get_ess_by_name(tmp_path):
    """Test lookup by name."""
    f = tmp_path / "plpess.dat"
    f.write_text(
        " 1\n"
        "    1  'Alpha'     1   50.0   50.0  0.95  0.95   4.0   0.50\n"
    )
    parser = EssParser(f)
    parser.parse()
    assert parser.get_ess_by_name("Alpha") is not None
    assert parser.get_ess_by_name("NoSuch") is None


def test_get_ess_by_number(tmp_path):
    """Test lookup by number."""
    f = tmp_path / "plpess.dat"
    f.write_text(
        " 1\n"
        "    5  'E5'        2   30.0   30.0  0.92  0.92   3.0   0.60\n"
    )
    parser = EssParser(f)
    parser.parse()
    assert parser.get_ess_by_number(5) is not None
    assert parser.get_ess_by_number(5)["name"] == "E5"
    assert parser.get_ess_by_number(99) is None


def test_parse_empty_file_raises(tmp_path):
    """Test that an empty file raises ValueError."""
    f = tmp_path / "plpess.dat"
    f.touch()
    parser = EssParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_missing_file_raises():
    """Test that a missing file raises FileNotFoundError on parse."""
    parser = EssParser("/nonexistent/plpess.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()
