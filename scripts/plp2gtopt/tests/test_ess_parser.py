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
    assert not parser.esses


def test_parse_single_ess(tmp_path):
    """Test parsing a single ESS entry (Nombre nc nd mloss emax dcmax)."""
    f = tmp_path / "plpess.dat"
    f.write_text(
        "# Numero de ESS\n"
        " 1\n"
        "# CenNom  nc  nd  mloss  Emax  DCMax  [DCMod CenPC]\n"
        "  ESS1    0.95  0.95   1.0   200.0  50.0   0\n"
    )
    parser = EssParser(f)
    parser.parse()

    assert parser.num_esses == 1
    e = parser.esses[0]
    assert e["name"] == "ESS1"
    assert e["nc"] == pytest.approx(0.95)
    assert e["nd"] == pytest.approx(0.95)
    assert e["mloss"] == pytest.approx(1.0)
    assert e["emax"] == pytest.approx(200.0)
    assert e["dcmax"] == pytest.approx(50.0)
    assert e["dcmod"] == 0
    assert e["cenpc"] == ""


def test_parse_ess_with_cenpc(tmp_path):
    """Test ESS entry with charge central name (coupled mode)."""
    f = tmp_path / "plpess.dat"
    f.write_text(" 1\n  ALFALFAL_ERD  0.9  0.9  1  244.3  59.3  1  ALFALFAL\n")
    parser = EssParser(f)
    parser.parse()

    assert parser.num_esses == 1
    e = parser.esses[0]
    assert e["name"] == "ALFALFAL_ERD"
    assert e["nc"] == pytest.approx(0.9)
    assert e["nd"] == pytest.approx(0.9)
    assert e["mloss"] == pytest.approx(1.0)
    assert e["emax"] == pytest.approx(244.3)
    assert e["dcmax"] == pytest.approx(59.3)
    assert e["dcmod"] == 1
    assert e["cenpc"] == "ALFALFAL"


def test_parse_multiple_esses(tmp_path):
    """Test parsing multiple ESS entries."""
    f = tmp_path / "plpess.dat"
    f.write_text(
        " 2\n"
        "  ESS1   0.95  0.95  0.0  200.0  50.0  0\n"
        "  ESS2   0.90  0.90  1.0  100.0  25.0  1  GEN_X\n"
    )
    parser = EssParser(f)
    parser.parse()

    assert parser.num_esses == 2
    assert parser.esses[0]["name"] == "ESS1"
    assert parser.esses[1]["name"] == "ESS2"
    assert parser.esses[1]["nc"] == pytest.approx(0.90)
    assert parser.esses[1]["emax"] == pytest.approx(100.0)
    assert parser.esses[1]["dcmod"] == 1
    assert parser.esses[1]["cenpc"] == "GEN_X"


def test_parse_with_comments(tmp_path):
    """Test that comment lines are skipped."""
    f = tmp_path / "plpess.dat"
    f.write_text("# Header\n 1\n# Row\n  MyESS  0.98  0.98  0.5  300.0  60.0  0\n")
    parser = EssParser(f)
    parser.parse()
    assert parser.num_esses == 1
    assert parser.esses[0]["name"] == "MyESS"


def test_get_ess_by_name(tmp_path):
    """Test lookup by name."""
    f = tmp_path / "plpess.dat"
    f.write_text(" 1\n  Alpha  0.95  0.95  0.0  200.0  50.0  0\n")
    parser = EssParser(f)
    parser.parse()
    assert parser.get_ess_by_name("Alpha") is not None
    assert parser.get_ess_by_name("NoSuch") is None


def test_parse_minimal_fields(tmp_path):
    """Test ESS with only 6 required fields (no dcmod, no cenpc)."""
    f = tmp_path / "plpess.dat"
    f.write_text(" 1\n  MinESS  0.90  0.85  2.0  150.0  30.0\n")
    parser = EssParser(f)
    parser.parse()
    e = parser.esses[0]
    assert e["name"] == "MinESS"
    assert e["dcmod"] == 0
    assert e["cenpc"] == ""


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
