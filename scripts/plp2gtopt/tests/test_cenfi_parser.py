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
    assert parser.num_filtrations == 0
    assert not parser.filtrations


def test_parse_single_entry(tmp_path):
    """Parsing a file with a single filtration entry."""
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

    assert parser.num_filtrations == 1
    entry = parser.filtrations[0]
    assert entry["name"] == "Reservoir1"
    assert entry["reservoir"] == "Reservoir1"
    assert entry["slope"] == pytest.approx(0.001)
    assert entry["constant"] == pytest.approx(5.0)


def test_parse_multiple_entries(tmp_path):
    """Parsing a file with two filtration entries."""
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

    assert parser.num_filtrations == 2
    assert parser.filtrations[0]["name"] == "Reservoir1"
    assert parser.filtrations[0]["slope"] == pytest.approx(0.001)
    assert parser.filtrations[1]["name"] == "TurbineGen"
    assert parser.filtrations[1]["slope"] == pytest.approx(0.0005)
    assert parser.filtrations[1]["constant"] == pytest.approx(2.5)


def test_parse_zero_slope_constant_only(tmp_path):
    """Parsing an entry with zero slope (constant seepage only)."""
    content = " 1\n'PlantA'\n'ReservoirA'\n0.0  3.14\n"
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    assert parser.num_filtrations == 1
    entry = parser.filtrations[0]
    assert entry["slope"] == pytest.approx(0.0)
    assert entry["constant"] == pytest.approx(3.14)


def test_parse_real_file():
    """Parse the actual test fixture file (2 entries)."""
    parser = CenfiParser(CENFI_TEST_FILE)
    parser.parse()

    assert parser.num_filtrations == 2
    e0 = parser.filtrations[0]
    assert e0["name"] == "Reservoir1"
    assert e0["reservoir"] == "Reservoir1"
    assert e0["slope"] == pytest.approx(0.001)
    assert e0["constant"] == pytest.approx(5.0)
    e1 = parser.filtrations[1]
    assert e1["name"] == "TurbineGen"
    assert e1["slope"] == pytest.approx(0.0005)
    assert e1["constant"] == pytest.approx(2.5)


# ─── Lookup tests ────────────────────────────────────────────────────────────


def test_get_filtration_by_central(tmp_path):
    """Lookup by central name works after parsing."""
    content = " 1\n'HYDRO1'\n'DAM1'\n0.002  1.0\n"
    f = write_cenfi(tmp_path, content)
    parser = CenfiParser(f)
    parser.parse()

    found = parser.get_filtration_by_central("HYDRO1")
    assert found is not None
    assert found["reservoir"] == "DAM1"
    assert found["slope"] == pytest.approx(0.002)

    not_found = parser.get_filtration_by_central("NONEXISTENT")
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
