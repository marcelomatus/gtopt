"""Unit tests for CenreParser class (plpcenre.dat parser)."""

from pathlib import Path
import pytest

from ..cenre_parser import CenreParser

# Test data file
CASES_DIR = Path(__file__).parent.parent.parent / "cases"
CENRE_TEST_FILE = CASES_DIR / "plp_min_cenre" / "plpcenre.dat"


# ─── Helper to write temp dat files ─────────────────────────────────────────


def write_cenre(tmp_path: Path, content: str) -> Path:
    """Write content to a temporary plpcenre.dat file."""
    f = tmp_path / "plpcenre.dat"
    f.write_text(content)
    return f


# ─── Basic parsing tests ─────────────────────────────────────────────────────


def test_parse_empty_file(tmp_path):
    """Parsing a file with 0 entries yields an empty list."""
    f = write_cenre(
        tmp_path,
        "# Archivo de Rendimiento de Embalses\n# Numero\n 0\n",
    )
    parser = CenreParser(f)
    parser.parse()
    assert parser.num_efficiencies == 0
    assert not parser.efficiencies


def test_parse_single_entry(tmp_path):
    """Parsing a file with a single entry with one segment."""
    content = (
        "# Archivo de Rendimiento de Embalses\n"
        " 1\n"
        "'Reservoir1'\n"
        "'Reservoir1'\n"
        "1.530\n"
        " 1\n"
        "     1    0.0000000     0.0002294    1.2558000   1.0E9\n"
    )
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    parser.parse()

    assert parser.num_efficiencies == 1
    entry = parser.efficiencies[0]
    assert entry["name"] == "Reservoir1"
    assert entry["reservoir"] == "Reservoir1"
    assert entry["mean_efficiency"] == pytest.approx(1.530)
    assert len(entry["segments"]) == 1
    seg = entry["segments"][0]
    assert seg["volume"] == pytest.approx(0.0)
    assert seg["slope"] == pytest.approx(0.0002294)
    assert seg["constant"] == pytest.approx(1.2558)


def test_parse_multiple_segments(tmp_path):
    """Parsing an entry with multiple piecewise-linear segments."""
    content = (
        " 1\n"
        "'TurbineA'\n"
        "'ReservoirA'\n"
        "2.100\n"
        " 2\n"
        "     1    0.0000000     0.0003000    1.8000000   1.0E9\n"
        "     2  500.0000000     0.0001000    2.1000000   1.0E9\n"
    )
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    parser.parse()

    assert parser.num_efficiencies == 1
    entry = parser.efficiencies[0]
    assert entry["mean_efficiency"] == pytest.approx(2.100)
    assert len(entry["segments"]) == 2
    assert entry["segments"][0]["slope"] == pytest.approx(0.0003)
    assert entry["segments"][1]["volume"] == pytest.approx(500000.0)
    assert entry["segments"][1]["constant"] == pytest.approx(2.1)


def test_fescala_volume_conversion(tmp_path):
    """FEscala physically scales volume breakpoints to dam³."""
    content = (
        " 1\n"
        "'T1'\n"
        "'R1'\n"
        "1.0\n"
        " 2\n"
        "     1  100.0     0.0003000    1.800   1.0E9\n"
        "     2  200.0     0.0001000    2.100   1.0E10\n"
    )
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    parser.parse()

    segs = parser.efficiencies[0]["segments"]
    # 100.0 × 1E9 / 1E6 = 100_000 dam³
    assert segs[0]["volume"] == pytest.approx(100_000.0)
    # 200.0 × 1E10 / 1E6 = 2_000_000 dam³
    assert segs[1]["volume"] == pytest.approx(2_000_000.0)
    # slope and constant unchanged
    assert segs[0]["slope"] == pytest.approx(0.0003)
    assert segs[1]["constant"] == pytest.approx(2.1)


def test_fescala_missing_defaults_to_1e6(tmp_path):
    """Without FEscala column, volume is stored as-is (×1E6/1E6 = ×1)."""
    content = " 1\n'T1'\n'R1'\n1.0\n 1\n     1  750.0     0.0002000    1.500\n"
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    parser.parse()

    seg = parser.efficiencies[0]["segments"][0]
    # 750.0 × 1E6 / 1E6 = 750.0 (passthrough)
    assert seg["volume"] == pytest.approx(750.0)


def test_parse_multiple_entries(tmp_path):
    """Parsing a file with two entries."""
    content = (
        " 2\n"
        "'COLBUN'\n"
        "'COLBUN'\n"
        "1.530\n"
        " 1\n"
        "     1    0.0000000     0.0002294    1.2558000   1.0E9\n"
        "'ELTORO'\n"
        "'ELTORO'\n"
        "4.800\n"
        " 1\n"
        "     1    0.0000000     0.0000900    4.5252000   1.0E10\n"
    )
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    parser.parse()

    assert parser.num_efficiencies == 2
    assert parser.efficiencies[0]["name"] == "COLBUN"
    assert parser.efficiencies[1]["name"] == "ELTORO"
    assert parser.efficiencies[1]["mean_efficiency"] == pytest.approx(4.800)


def test_parse_real_file():
    """Parse the actual test fixture file (2 entries, 1 and 2 segments)."""
    parser = CenreParser(CENRE_TEST_FILE)
    parser.parse()

    assert parser.num_efficiencies == 2
    # Entry 0: Reservoir1, 1 segment
    e0 = parser.efficiencies[0]
    assert e0["name"] == "Reservoir1"
    assert e0["reservoir"] == "Reservoir1"
    assert e0["mean_efficiency"] == pytest.approx(1.530)
    assert len(e0["segments"]) == 1
    # Entry 1: TurbineGen, 2 segments
    e1 = parser.efficiencies[1]
    assert e1["name"] == "TurbineGen"
    assert len(e1["segments"]) == 2


# ─── Lookup tests ────────────────────────────────────────────────────────────


def test_get_efficiency_by_central(tmp_path):
    """Lookup by central name works after parsing."""
    content = (
        " 1\n"
        "'RAPEL'\n"
        "'RAPEL'\n"
        "0.635\n"
        " 1\n"
        "     1    0.0000000     0.0001000    0.5734000   1.0E9\n"
    )
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    parser.parse()

    found = parser.get_efficiency_by_central("RAPEL")
    assert found is not None
    assert found["mean_efficiency"] == pytest.approx(0.635)

    not_found = parser.get_efficiency_by_central("NONEXISTENT")
    assert not_found is None


# ─── Error handling tests ─────────────────────────────────────────────────────


def test_parse_nonexistent_file(tmp_path):
    """Parsing a nonexistent file raises FileNotFoundError."""
    parser = CenreParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_parse_too_few_segment_fields(tmp_path):
    """A segment line with too few fields raises ValueError."""
    content = (
        " 1\n"
        "'X'\n"
        "'Y'\n"
        "1.0\n"
        " 1\n"
        "     1    0.0\n"  # Only 2 fields, need at least 4
    )
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    with pytest.raises(ValueError, match="too few fields"):
        parser.parse()


def test_parse_unexpected_eof(tmp_path):
    """Unexpected EOF during central name line raises ValueError."""
    content = " 1\n"  # Claims 1 entry but provides nothing
    f = write_cenre(tmp_path, content)
    parser = CenreParser(f)
    with pytest.raises(ValueError, match="Unexpected end"):
        parser.parse()
