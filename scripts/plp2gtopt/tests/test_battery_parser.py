"""Unit tests for BatteryParser class."""

from pathlib import Path
import pytest

from ..battery_parser import BatteryParser


def test_battery_parser_initialization():
    """Test BatteryParser initialization."""
    parser = BatteryParser("test.dat")
    assert parser.file_path == Path("test.dat")
    assert not parser.batteries
    assert parser.num_batteries == 0


def test_parse_zero_batteries(tmp_path):
    """Test parsing a file with 0 battery entries."""
    f = tmp_path / "plpcenbat.dat"
    f.write_text("# Numero de baterias total, Numero maximo de inyecciones\n 0  1\n")
    parser = BatteryParser(f)
    parser.parse()
    assert parser.num_batteries == 0
    assert not parser.batteries


def test_parse_single_battery(tmp_path):
    """Test parsing a single battery entry."""
    f = tmp_path / "plpcenbat.dat"
    f.write_text(
        "# header\n"
        " 1     1\n"
        "# Baterias\n"
        "     1     BESS1\n"
        "     # Numero de centrales que inyectan\n"
        "          1\n"
        "     # Central que inyecta, Factor de perdida de carga\n"
        "     BESS1_NEG     0.95\n"
        "# Barra, FPD, EMin, EMax\n"
        "     1     0.95     0.0     200.0\n"
    )
    parser = BatteryParser(f)
    parser.parse()

    assert parser.num_batteries == 1
    b = parser.batteries[0]
    assert b["number"] == 1
    assert b["name"] == "BESS1"
    assert b["bus"] == 1
    assert b["fpd"] == pytest.approx(0.95)
    assert b["emin"] == pytest.approx(0.0)
    assert b["emax"] == pytest.approx(200.0)
    assert len(b["injections"]) == 1
    assert b["injections"][0]["name"] == "BESS1_NEG"
    assert b["injections"][0]["fpc"] == pytest.approx(0.95)


def test_parse_multiple_injections(tmp_path):
    """Test parsing a battery with multiple injection centrals."""
    f = tmp_path / "plpcenbat.dat"
    f.write_text(
        " 1     2\n"
        " 1     BAT1\n"
        " 2\n"
        " SOLAR1     0.95\n"
        " WIND1      0.90\n"
        " 10     0.92     10.0     500.0\n"
    )
    parser = BatteryParser(f)
    parser.parse()

    assert parser.num_batteries == 1
    b = parser.batteries[0]
    assert b["name"] == "BAT1"
    assert len(b["injections"]) == 2
    assert b["injections"][0]["name"] == "SOLAR1"
    assert b["injections"][1]["name"] == "WIND1"
    assert b["injections"][1]["fpc"] == pytest.approx(0.90)
    assert b["bus"] == 10
    assert b["emax"] == pytest.approx(500.0)
    assert b["emin"] == pytest.approx(10.0)


def test_parse_multiple_batteries(tmp_path):
    """Test parsing multiple battery entries."""
    f = tmp_path / "plpcenbat.dat"
    f.write_text(
        " 2     1\n"
        " 1     BAT_A\n"
        " 1\n"
        " INJ_A     0.90\n"
        " 5     0.90     0.0     100.0\n"
        " 2     BAT_B\n"
        " 1\n"
        " INJ_B     0.95\n"
        " 6     0.92     5.0     200.0\n"
    )
    parser = BatteryParser(f)
    parser.parse()

    assert parser.num_batteries == 2
    assert parser.batteries[0]["name"] == "BAT_A"
    assert parser.batteries[1]["name"] == "BAT_B"
    assert parser.batteries[1]["bus"] == 6
    assert parser.batteries[1]["emax"] == pytest.approx(200.0)


def test_parse_with_comments(tmp_path):
    """Test that comment lines are properly skipped."""
    f = tmp_path / "plpcenbat.dat"
    f.write_text(
        "# Archivo de baterias\n"
        "# Num baterias, max inyecciones\n"
        " 1     1\n"
        "# Baterias\n"
        " 1     MyBat\n"
        "     # Inyecciones\n"
        "          1\n"
        "     # Central, FPC\n"
        "     MyBat_NEG     0.88\n"
        "# Barra, FPD, EMin, EMax\n"
        " 3     0.91     2.0     150.0\n"
    )
    parser = BatteryParser(f)
    parser.parse()
    assert parser.num_batteries == 1
    b = parser.batteries[0]
    assert b["name"] == "MyBat"
    assert b["bus"] == 3
    assert b["fpd"] == pytest.approx(0.91)
    assert b["emax"] == pytest.approx(150.0)


def test_get_battery_by_name(tmp_path):
    """Test lookup by name."""
    f = tmp_path / "plpcenbat.dat"
    f.write_text(
        " 1     1\n"
        " 1     Alpha\n"
        " 1\n"
        " AlphaChg     0.95\n"
        " 1     0.95     0.0     100.0\n"
    )
    parser = BatteryParser(f)
    parser.parse()
    bat = parser.get_battery_by_name("Alpha")
    assert bat is not None
    assert bat["number"] == 1
    assert parser.get_battery_by_name("NoSuch") is None


def test_get_battery_by_number(tmp_path):
    """Test lookup by number."""
    f = tmp_path / "plpcenbat.dat"
    f.write_text(
        " 1     1\n"
        " 7     BatSeven\n"
        " 1\n"
        " BatSeven_C     0.90\n"
        " 3     0.90     0.0     50.0\n"
    )
    parser = BatteryParser(f)
    parser.parse()
    bat = parser.get_battery_by_number(7)
    assert bat is not None
    assert bat["name"] == "BatSeven"
    assert parser.get_battery_by_number(99) is None


def test_parse_empty_file_raises(tmp_path):
    """Test that an empty file raises ValueError."""
    f = tmp_path / "plpcenbat.dat"
    f.touch()
    parser = BatteryParser(f)
    with pytest.raises(ValueError):
        parser.parse()


def test_missing_file_raises():
    """Test that a missing file raises FileNotFoundError on parse."""
    parser = BatteryParser("/nonexistent/plpcenbat.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()
