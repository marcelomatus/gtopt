"""Tests for CenpmaxParser — plpcenpmax.dat parsing."""

from pathlib import Path

import pytest

from plp2gtopt.cenpmax_parser import CenpmaxParser

CASES_DIR = Path(__file__).parent.parent.parent / "cases"


def write_cenpmax(tmp_path: Path, content: str) -> Path:
    """Write content to a temporary plpcenpmax.dat file."""
    f = tmp_path / "plpcenpmax.dat"
    f.write_text(content)
    return f


# ─── Basic parsing (default: raw units) ─────────────────────────────────────


def test_parse_empty(tmp_path):
    """File with 0 entries yields an empty list."""
    f = write_cenpmax(tmp_path, "# header\n# header2\n0\n")
    parser = CenpmaxParser(f)
    parser.parse()
    assert parser.num_pmax_curves == 0


def test_parse_single_entry_raw(tmp_path):
    """Single entry with 2 segments, raw units (default)."""
    content = (
        "# header\n"
        "1\n"
        "'CIPRESES'\n"
        "'CIPRESES'\n"
        "2\n"
        "0.0     0.295071481   30.00000000\n"
        "4.7     0.209379029   87.75930472\n"
    )
    f = write_cenpmax(tmp_path, content)
    parser = CenpmaxParser(f)
    parser.parse()

    assert parser.num_pmax_curves == 1
    entry = parser.pmax_curves[0]
    assert entry["name"] == "CIPRESES"
    assert entry["reservoir"] == "CIPRESES"
    assert len(entry["segments"]) == 2

    seg0 = entry["segments"][0]
    # Raw: volume=0.0 Mm³, slope=0.295 MW/Mm³, constant=30 MW
    assert seg0["volume"] == pytest.approx(0.0)
    assert seg0["slope"] == pytest.approx(0.295071481)
    assert seg0["constant"] == pytest.approx(30.0)

    seg1 = entry["segments"][1]
    assert seg1["volume"] == pytest.approx(4.7)
    assert seg1["constant"] == pytest.approx(87.75930472)


def test_parse_with_convert_units(tmp_path):
    """With convert_units=True, volume ×1000 and slope ÷1000."""
    content = (
        "1\n"
        "'TUR1'\n"
        "'RES1'\n"
        "2\n"
        "0.0     0.295071481   30.00000000\n"
        "4.7     0.209379029   87.75930472\n"
    )
    f = write_cenpmax(tmp_path, content)
    parser = CenpmaxParser(f)
    parser.parse(convert_units=True)

    segs = parser.pmax_curves[0]["segments"]
    # volume: 4.7 × 1000 = 4700 dam³
    assert segs[1]["volume"] == pytest.approx(4700.0)
    # slope: 0.295071481 / 1000 = 0.000295071481 MW/dam³
    assert segs[0]["slope"] == pytest.approx(0.000295071481)
    # constant unchanged
    assert segs[0]["constant"] == pytest.approx(30.0)


def test_parse_multiple_entries(tmp_path):
    """Multiple entries parsed correctly (raw units)."""
    content = (
        "2\n"
        "'CENTRAL_A'\n"
        "'RESERVOIR_A'\n"
        "1\n"
        "100.0  0.5  200.0\n"
        "'CENTRAL_B'\n"
        "'RESERVOIR_B'\n"
        "1\n"
        "50.0  0.3  150.0\n"
    )
    f = write_cenpmax(tmp_path, content)
    parser = CenpmaxParser(f)
    parser.parse()

    assert parser.num_pmax_curves == 2
    assert parser.pmax_curves[0]["name"] == "CENTRAL_A"
    assert parser.pmax_curves[1]["name"] == "CENTRAL_B"

    # Raw units: volume=100.0 Mm³, slope=0.3 MW/Mm³
    assert parser.pmax_curves[0]["segments"][0]["volume"] == pytest.approx(100.0)
    assert parser.pmax_curves[1]["segments"][0]["slope"] == pytest.approx(0.3)


def test_lookup_by_central(tmp_path):
    """get_pmax_by_central returns correct entry."""
    content = "1\n'TUR1'\n'RES1'\n1\n10.0  0.1  50.0\n"
    f = write_cenpmax(tmp_path, content)
    parser = CenpmaxParser(f)
    parser.parse()

    assert parser.get_pmax_by_central("TUR1") is not None
    assert parser.get_pmax_by_central("MISSING") is None


def test_parse_real_case():
    """Parse the plp_case_2y fixture (4 entries, raw units)."""
    from plp2gtopt.compressed_open import resolve_compressed_path

    path = CASES_DIR / "plp_case_2y" / "plpcenpmax.dat"
    try:
        path = resolve_compressed_path(path)
    except FileNotFoundError:
        pytest.skip("plp_case_2y not available")
    parser = CenpmaxParser(path)
    parser.parse()

    assert parser.num_pmax_curves == 4
    names = [e["name"] for e in parser.pmax_curves]
    assert "CIPRESES" in names
    assert "COLBUN" in names
    assert "RALCO" in names
    assert "CANUTILLAR" in names

    # COLBUN first segment: raw volume=381.6 Mm³
    colbun = parser.get_pmax_by_central("COLBUN")
    assert colbun is not None
    assert colbun["segments"][0]["volume"] == pytest.approx(381.6)
    assert colbun["segments"][0]["constant"] == pytest.approx(239.7617143)


def test_parse_real_case_converted():
    """Parse real fixture with convert_units=True."""
    from plp2gtopt.compressed_open import resolve_compressed_path

    path = CASES_DIR / "plp_case_2y" / "plpcenpmax.dat"
    try:
        path = resolve_compressed_path(path)
    except FileNotFoundError:
        pytest.skip("plp_case_2y not available")
    parser = CenpmaxParser(path)
    parser.parse(convert_units=True)

    colbun = parser.get_pmax_by_central("COLBUN")
    assert colbun is not None
    # 381.6 × 1000 = 381600 dam³
    assert colbun["segments"][0]["volume"] == pytest.approx(381600.0)


# ─── Error handling ─────────────────────────────────────────────────────────


def test_nonexistent_file(tmp_path):
    """Nonexistent file raises FileNotFoundError."""
    parser = CenpmaxParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()


def test_too_few_segment_fields(tmp_path):
    """Segment line with too few fields raises ValueError."""
    content = "1\n'X'\n'Y'\n1\n10.0  0.1\n"
    f = write_cenpmax(tmp_path, content)
    parser = CenpmaxParser(f)
    with pytest.raises(ValueError, match="too few fields"):
        parser.parse()


def test_unexpected_eof(tmp_path):
    """Unexpected EOF raises ValueError."""
    content = "1\n'X'\n"
    f = write_cenpmax(tmp_path, content)
    parser = CenpmaxParser(f)
    with pytest.raises(ValueError, match="Unexpected end"):
        parser.parse()
