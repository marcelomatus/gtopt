"""Unit tests for FilembParser class (plpfilemb.dat parser)."""

from pathlib import Path
import pytest

from ..filemb_parser import FilembParser

# Test data file
CASES_DIR = Path(__file__).parent.parent.parent / "cases"
FILEMB_TEST_FILE = CASES_DIR / "plp_min_filemb" / "plpfilemb.dat"


# ─── Helper to write temp dat files ─────────────────────────────────────────


def write_filemb(tmp_path: Path, content: str) -> Path:
    """Write content to a temporary plpfilemb.dat file."""
    f = tmp_path / "plpfilemb.dat"
    f.write_text(content)
    return f


# ─── Basic parsing tests ─────────────────────────────────────────────────────


def test_parse_empty_file(tmp_path):
    """Parsing a file with 0 entries yields an empty list."""
    f = write_filemb(
        tmp_path,
        "# Archivo de Filtraciones de Embalses\n# Numero\n 0\n",
    )
    parser = FilembParser(f)
    parser.parse()
    assert parser.num_seepages == 0
    assert not parser.seepages


def test_parse_single_entry_one_segment(tmp_path):
    """Parsing a file with a single entry and one segment."""
    content = (
        "# Archivo de Filtraciones de Embalses\n"
        " 1\n"
        "'Reservoir1'\n"
        "5.0\n"
        " 1\n"
        "     1    0.0     0.16131527    2.189179627\n"
        "'Central1'\n"
    )
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    parser.parse()

    assert parser.num_seepages == 1
    entry = parser.seepages[0]
    assert entry["embalse"] == "Reservoir1"
    assert entry["central"] == "Central1"
    assert entry["mean_seepage"] == pytest.approx(5.0)
    assert len(entry["segments"]) == 1

    seg = entry["segments"][0]
    # Volume: 0.0 Mm³ (= hm³, same as gtopt physical unit)
    assert seg["volume"] == pytest.approx(0.0)
    # Slope: 0.16131527 m³/s per hm³ (no conversion)
    assert seg["slope"] == pytest.approx(0.16131527)
    # Constant: no conversion
    assert seg["constant"] == pytest.approx(2.189179627)


def test_parse_single_entry_multiple_segments(tmp_path):
    """Parsing an entry with multiple piecewise-linear segments."""
    content = (
        " 1\n"
        "'ELTORO'\n"
        "30.80\n"
        " 3\n"
        "     1     0.0     0.043150359    0.0000000\n"
        "     2   400.0     0.005429197    15.08846449\n"
        "     3  2700.0     0.007149312    10.44415451\n"
        "'ABANICO'\n"
    )
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    parser.parse()

    assert parser.num_seepages == 1
    entry = parser.seepages[0]
    assert entry["embalse"] == "ELTORO"
    assert entry["central"] == "ABANICO"
    assert entry["mean_seepage"] == pytest.approx(30.80)
    assert len(entry["segments"]) == 3

    # Segment 1: volume = 0.0 [hm³] (no conversion)
    assert entry["segments"][0]["volume"] == pytest.approx(0.0)
    assert entry["segments"][0]["slope"] == pytest.approx(0.043150359)
    assert entry["segments"][0]["constant"] == pytest.approx(0.0)

    # Segment 2: volume = 400.0 [hm³]
    assert entry["segments"][1]["volume"] == pytest.approx(400.0)
    assert entry["segments"][1]["slope"] == pytest.approx(0.005429197)
    assert entry["segments"][1]["constant"] == pytest.approx(15.08846449)

    # Segment 3: volume = 2700.0 [hm³]
    assert entry["segments"][2]["volume"] == pytest.approx(2700.0)
    assert entry["segments"][2]["slope"] == pytest.approx(0.007149312)
    assert entry["segments"][2]["constant"] == pytest.approx(10.44415451)


def test_parse_multiple_entries(tmp_path):
    """Parsing a file with two entries."""
    content = (
        " 2\n"
        "'CIPRESES'\n"
        "14.20\n"
        " 1\n"
        "     1     0.0     0.16131527    2.189179627\n"
        "'FILT_CIPRESES'\n"
        "'COLBUN'\n"
        "6.10\n"
        " 2\n"
        "     1     0.0     0.000000000   0.000000000\n"
        "     2   660.6     0.004061368  -1.868229129\n"
        "'SAN_CLEMENTE'\n"
    )
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    parser.parse()

    assert parser.num_seepages == 2
    e0 = parser.seepages[0]
    assert e0["embalse"] == "CIPRESES"
    assert e0["central"] == "FILT_CIPRESES"
    assert e0["mean_seepage"] == pytest.approx(14.20)
    assert len(e0["segments"]) == 1

    e1 = parser.seepages[1]
    assert e1["embalse"] == "COLBUN"
    assert e1["central"] == "SAN_CLEMENTE"
    assert e1["mean_seepage"] == pytest.approx(6.10)
    assert len(e1["segments"]) == 2
    # Volume: 660.6 [hm³] (no conversion)
    assert e1["segments"][1]["volume"] == pytest.approx(660.6)


def test_parse_real_file():
    """Parse the actual test fixture file (2 entries)."""
    parser = FilembParser(FILEMB_TEST_FILE)
    parser.parse()

    assert parser.num_seepages == 2

    e0 = parser.seepages[0]
    assert e0["embalse"] == "Reservoir1"
    assert e0["central"] == "Central1"
    assert e0["mean_seepage"] == pytest.approx(5.0)
    assert len(e0["segments"]) == 1
    # Volume: 0.0 [hm³] (no conversion)
    assert e0["segments"][0]["volume"] == pytest.approx(0.0)
    # Slope: 0.16131527 [m³/s per hm³] (no conversion)
    assert e0["segments"][0]["slope"] == pytest.approx(0.16131527)
    assert e0["segments"][0]["constant"] == pytest.approx(2.189179627)

    e1 = parser.seepages[1]
    assert e1["embalse"] == "Reservoir2"
    assert e1["central"] == "Central2"
    assert e1["mean_seepage"] == pytest.approx(12.5)
    assert len(e1["segments"]) == 2


def test_parse_with_comments(tmp_path):
    """Lines starting with # are treated as comments and skipped."""
    content = (
        "# Header comment\n"
        "# Numero Embalses\n"
        " 1\n"
        "# Nombre embalse\n"
        "'MyReservoir'\n"
        "# Filtracion media\n"
        "3.5\n"
        "# Numero tramos\n"
        " 2\n"
        "# Tramo Vol Pend Const\n"
        "     1   0.0   0.001   0.5\n"
        "     2  50.0   0.002   1.0\n"
        "# Destino\n"
        "'Downstream'\n"
    )
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    parser.parse()

    assert parser.num_seepages == 1
    entry = parser.seepages[0]
    assert entry["embalse"] == "MyReservoir"
    assert entry["central"] == "Downstream"
    assert entry["mean_seepage"] == pytest.approx(3.5)
    assert len(entry["segments"]) == 2
    # Segment 2: volume = 50.0 [hm³]
    assert entry["segments"][1]["volume"] == pytest.approx(50.0)


def test_negative_mean_seepage_clipped(tmp_path):
    """Negative mean_seepage is clipped to 0 (matching PLP Fortran)."""
    content = (
        " 1\n"
        "'RSV'\n"
        "-5.0\n"  # Negative value → should be clipped to 0
        " 1\n"
        "     1   0.0   0.001   0.5\n"
        "'CTR'\n"
    )
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    parser.parse()

    assert parser.seepages[0]["mean_seepage"] == pytest.approx(0.0)


def test_no_unit_conversions_volume_and_slope(tmp_path):
    """Verify volumes and slopes pass through unchanged (already in hm³)."""
    content = " 1\n'RSV'\n1.0\n 1\n     1   100.0   2.0   3.0\n'CTR'\n"
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    parser.parse()

    seg = parser.seepages[0]["segments"][0]
    assert seg["volume"] == pytest.approx(100.0)  # 100 [hm³] unchanged
    assert seg["slope"] == pytest.approx(2.0)  # [m³/s per hm³] unchanged
    assert seg["constant"] == pytest.approx(3.0)  # [m³/s] unchanged


# ─── Lookup tests ─────────────────────────────────────────────────────────────


def test_get_seepage_by_embalse(tmp_path):
    """Lookup by embalse name works after parsing."""
    content = " 1\n'RAPEL'\n8.0\n 1\n     1   0.0   0.1   1.0\n'DONOSO'\n"
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    parser.parse()

    found = parser.get_seepage_by_embalse("RAPEL")
    assert found is not None
    assert found["mean_seepage"] == pytest.approx(8.0)

    not_found = parser.get_seepage_by_embalse("NONEXISTENT")
    assert not_found is None


# ─── Error handling tests ─────────────────────────────────────────────────────


def test_parse_nonexistent_file(tmp_path):
    """Parsing a nonexistent file raises FileNotFoundError."""
    parser = FilembParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_parse_too_few_segment_fields(tmp_path):
    """A segment line with too few fields raises ValueError."""
    content = (
        " 1\n"
        "'X'\n"
        "1.0\n"
        " 1\n"
        "     1    0.0\n"  # Only 2 fields, need at least 4
        "'Y'\n"
    )
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    with pytest.raises(ValueError, match="too few fields"):
        parser.parse()


def test_parse_unexpected_eof(tmp_path):
    """Unexpected EOF after embalse name raises ValueError."""
    content = " 1\n'RSV'\n"  # Claims 1 entry but missing fields
    f = write_filemb(tmp_path, content)
    parser = FilembParser(f)
    with pytest.raises(ValueError, match="Unexpected end"):
        parser.parse()
