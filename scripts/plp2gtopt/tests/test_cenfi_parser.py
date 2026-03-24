"""Unit tests for CenfiParser class (plpcenfi.dat parser)."""

from pathlib import Path
import pytest

from ..cenfi_parser import CenfiParser

# Test data file
CASES_DIR = Path(__file__).parent.parent.parent / "cases"
CENFI_TEST_FILE = CASES_DIR / "plp_min_cenre" / "plpcenfi.dat"


# ─── Helper to write temp dat files ─────────────────────────────────────────


def write_cenfi(tmp_path: Path, content: str) -> Path:
    """Write content to a temporary plpcenfi.dat file."""
    f = tmp_path / "plpcenfi.dat"
    f.write_text(content)
    return f


# ─── Basic parsing tests ─────────────────────────────────────────────────────


def test_parse_empty_file(tmp_path):
    """Parsing a file with 0 entries yields an empty list."""
    f = write_cenfi(
        tmp_path,
        "# Archivo de centrales filtracion\n# Numero\n 0\n",
    )
    parser = CenfiParser(f)
    parser.parse()
    assert parser.num_seepages == 0
    assert not parser.seepages


def test_parse_single_entry(tmp_path):
    """Parsing a file with a single seepage entry."""
    content = (
        "# Archivo de centrales filtracion\n"
        " 1\n"
        "'Reservoir1'\n"
        "'Reservoir1'\n"
        "0.001  5.0\n"
    )
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    assert parser.num_seepages == 1
    entry = parser.seepages[0]
    assert entry["name"] == "Reservoir1"
    assert entry["reservoir"] == "Reservoir1"
    assert entry["slope"] == pytest.approx(0.001)
    assert entry["constant"] == pytest.approx(5.0)


def test_parse_multiple_entries(tmp_path):
    """Parsing a file with two seepage entries."""
    content = (
        " 2\n"
        "'Reservoir1'\n"
        "'Reservoir1'\n"
        "0.001  5.0\n"
        "'TurbineGen'\n"
        "'Reservoir1'\n"
        "0.0005  2.5\n"
    )
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    assert parser.num_seepages == 2
    assert parser.seepages[0]["name"] == "Reservoir1"
    assert parser.seepages[0]["slope"] == pytest.approx(0.001)
    assert parser.seepages[1]["name"] == "TurbineGen"
    assert parser.seepages[1]["slope"] == pytest.approx(0.0005)
    assert parser.seepages[1]["constant"] == pytest.approx(2.5)


def test_parse_zero_slope_constant_only(tmp_path):
    """Parsing an entry with zero slope (constant seepage only)."""
    content = " 1\n'PlantA'\n'ReservoirA'\n0.0  3.14\n"
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    assert parser.num_seepages == 1
    entry = parser.seepages[0]
    assert entry["slope"] == pytest.approx(0.0)
    assert entry["constant"] == pytest.approx(3.14)


def test_parse_real_file():
    """Parse the actual test fixture file (2 entries)."""
    parser = CenfiParser(CENFI_TEST_FILE)
    parser.parse()

    assert parser.num_seepages == 2
    e0 = parser.seepages[0]
    assert e0["name"] == "Reservoir1"
    assert e0["reservoir"] == "Reservoir1"
    assert e0["slope"] == pytest.approx(0.001)
    assert e0["constant"] == pytest.approx(5.0)
    e1 = parser.seepages[1]
    assert e1["name"] == "TurbineGen"
    assert e1["slope"] == pytest.approx(0.0005)
    assert e1["constant"] == pytest.approx(2.5)


# ─── Lookup tests ────────────────────────────────────────────────────────────


def test_get_seepage_by_central(tmp_path):
    """Lookup by central name works after parsing."""
    content = " 1\n'HYDRO1'\n'DAM1'\n0.002  1.0\n"
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    found = parser.get_seepage_by_central("HYDRO1")
    assert found is not None
    assert found["reservoir"] == "DAM1"
    assert found["slope"] == pytest.approx(0.002)

    not_found = parser.get_seepage_by_central("NONEXISTENT")
    assert not_found is None


# ─── Error handling tests ─────────────────────────────────────────────────────


def test_parse_nonexistent_file(tmp_path):
    """Parsing a nonexistent file raises FileNotFoundError."""
    parser = CenfiParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_parse_too_few_fields(tmp_path):
    """A slope/constant line with only one field raises ValueError."""
    content = (
        " 1\n"
        "'X'\n"
        "'Y'\n"
        "0.001\n"  # Missing constant
    )
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    with pytest.raises(ValueError, match="too few fields"):
        parser.parse()


def test_parse_unexpected_eof(tmp_path):
    """Unexpected EOF during parsing raises ValueError."""
    content = " 1\n"  # Claims 1 entry but provides nothing
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    with pytest.raises(ValueError, match="Unexpected end"):
        parser.parse()


# ─── Piecewise segment tests ────────────────────────────────────────────────


def test_parse_extended_format_with_segments(tmp_path):
    """Parse the extended format with piecewise-linear segments."""
    content = (
        " 1\n'Reservoir1'\n'Dam1'\n 2\n 1  0.0    0.001  2.0\n 2  500.0  0.0002 2.8\n"
    )
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    assert parser.num_seepages == 1
    entry = parser.seepages[0]
    assert entry["name"] == "Reservoir1"
    assert entry["reservoir"] == "Dam1"
    # Default slope/constant from first segment
    assert entry["slope"] == pytest.approx(0.001)
    assert entry["constant"] == pytest.approx(2.0)
    # Segments
    assert len(entry["segments"]) == 2
    assert entry["segments"][0]["volume"] == pytest.approx(0.0)
    assert entry["segments"][0]["slope"] == pytest.approx(0.001)
    assert entry["segments"][0]["constant"] == pytest.approx(2.0)
    assert entry["segments"][1]["volume"] == pytest.approx(500.0)
    assert entry["segments"][1]["slope"] == pytest.approx(0.0002)
    assert entry["segments"][1]["constant"] == pytest.approx(2.8)


def test_parse_extended_format_zero_segments(tmp_path):
    """Parse the extended format with zero segments."""
    content = " 1\n'PlantX'\n'DamX'\n 0\n"
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    assert parser.num_seepages == 1
    entry = parser.seepages[0]
    assert entry["slope"] == pytest.approx(0.0)
    assert entry["constant"] == pytest.approx(0.0)
    assert entry["segments"] == []


def test_legacy_format_includes_empty_segments(tmp_path):
    """Legacy format entries always have an empty segments list."""
    content = " 1\n'Plant'\n'Dam'\n0.001  5.0\n"
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    entry = parser.seepages[0]
    assert entry["segments"] == []


def test_mixed_legacy_and_extended(tmp_path):
    """Parse a file mixing legacy and extended format entries."""
    content = (
        " 2\n"
        "# Legacy entry\n"
        "'LegacyPlant'\n"
        "'LegacyDam'\n"
        "0.001  5.0\n"
        "# Extended entry\n"
        "'ExtPlant'\n"
        "'ExtDam'\n"
        " 2\n"
        " 1  0.0    0.002  1.0\n"
        " 2  1000.0 0.001  3.0\n"
    )
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    assert parser.num_seepages == 2
    # Legacy entry
    e0 = parser.seepages[0]
    assert e0["slope"] == pytest.approx(0.001)
    assert e0["constant"] == pytest.approx(5.0)
    assert e0["segments"] == []
    # Extended entry
    e1 = parser.seepages[1]
    assert len(e1["segments"]) == 2
    assert e1["segments"][0]["volume"] == pytest.approx(0.0)
    assert e1["segments"][1]["volume"] == pytest.approx(1000.0)


def test_parse_segment_too_few_fields(tmp_path):
    """A segment line with too few fields raises ValueError."""
    content = (
        " 1\n"
        "'X'\n"
        "'Y'\n"
        " 1\n"
        " 1  0.0\n"  # Missing slope and constant
    )
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    with pytest.raises(ValueError, match="too few fields"):
        parser.parse()
