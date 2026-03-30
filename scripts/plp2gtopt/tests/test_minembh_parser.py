"""Tests for MinembhParser — plpminembh.dat parsing."""

from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

from plp2gtopt.minembh_parser import MinembhParser


def _make_central_parser(*reservoirs):
    """Build a fake central_parser with get_central_by_name()."""
    data = {name: {"energy_scale": scale} for name, scale in reservoirs}

    def get_central_by_name(name):
        return data.get(name)

    return SimpleNamespace(get_central_by_name=get_central_by_name)


CASES_DIR = Path(__file__).parent.parent.parent / "cases"


# ─── Basic parsing ──────────────────────────────────────────────────────────


def test_parse_empty(tmp_path):
    """File with 0 reservoirs yields an empty list."""
    p = tmp_path / "plpminembh.dat"
    p.write_text("# header\n0\n")
    parser = MinembhParser(p)
    parser.parse()
    assert parser.num_minembhs == 0


def test_parse_single_reservoir(tmp_path):
    """Single reservoir with 3 stages, no central_parser (scale=1)."""
    content = (
        "# header\n1\n'RES1'\n3\n  1  100.0  10.0\n  2  200.0  10.0\n  3  300.0  10.0\n"
    )
    p = tmp_path / "plpminembh.dat"
    p.write_text(content)
    parser = MinembhParser(p)
    parser.parse()

    assert parser.num_minembhs == 1
    entry = parser.minembhs[0]
    assert entry["name"] == "RES1"
    np.testing.assert_array_equal(entry["stage"], [1, 2, 3])
    np.testing.assert_allclose(entry["vmin"], [100.0, 200.0, 300.0])
    np.testing.assert_allclose(entry["cost"], [10.0, 10.0, 10.0])


def test_parse_with_energy_scale(tmp_path):
    """vmin values are scaled by energy_scale from central_parser."""
    content = "# header\n1\n'COLBUN'\n2\n  4  0.5  10.0\n  5  1.0  10.0\n"
    p = tmp_path / "plpminembh.dat"
    p.write_text(content)
    # energy_scale = EmbFEsc / 1E6 = 1E9 / 1E6 = 1000
    cp = _make_central_parser(("COLBUN", 1000.0))
    parser = MinembhParser(p)
    parser.parse({"central_parser": cp})

    entry = parser.minembhs[0]
    np.testing.assert_allclose(entry["vmin"], [500.0, 1000.0])
    # cost is not scaled
    np.testing.assert_allclose(entry["cost"], [10.0, 10.0])


def test_parse_multiple_reservoirs(tmp_path):
    """Multiple reservoirs parsed correctly."""
    content = (
        "# header\n"
        "2\n"
        "'RES_A'\n"
        "1\n"
        "  1  50.0  5.0\n"
        "'RES_B'\n"
        "2\n"
        "  1  10.0  8.0\n"
        "  2  20.0  8.0\n"
    )
    p = tmp_path / "plpminembh.dat"
    p.write_text(content)
    parser = MinembhParser(p)
    parser.parse()

    assert parser.num_minembhs == 2
    assert parser.minembhs[0]["name"] == "RES_A"
    assert parser.minembhs[1]["name"] == "RES_B"
    assert len(parser.minembhs[1]["stage"]) == 2


def test_lookup_by_name(tmp_path):
    """get_minembh_by_name returns correct entry."""
    content = "# h\n1\n'ABC'\n1\n  1  42.0  7.0\n"
    p = tmp_path / "plpminembh.dat"
    p.write_text(content)
    parser = MinembhParser(p)
    parser.parse()

    assert parser.get_minembh_by_name("ABC") is not None
    assert parser.get_minembh_by_name("MISSING") is None


def test_parse_real_case():
    """Parse the plp_case_2y fixture (3 reservoirs)."""
    from plp2gtopt.compressed_open import resolve_compressed_path

    path = CASES_DIR / "plp_case_2y" / "plpminembh.dat"
    try:
        path = resolve_compressed_path(path)
    except FileNotFoundError:
        pytest.skip("plp_case_2y not available")
    parser = MinembhParser(path)
    # No central_parser → scale=1 (raw values)
    parser.parse()
    assert parser.num_minembhs == 3
    names = [e["name"] for e in parser.minembhs]
    assert "COLBUN" in names
    assert "RAPEL" in names
    assert "CANUTILLAR" in names


def test_parse_zero_stages_skipped(tmp_path):
    """Reservoir with 0 stages is skipped."""
    content = "# h\n1\n'SKIP'\n0\n"
    p = tmp_path / "plpminembh.dat"
    p.write_text(content)
    parser = MinembhParser(p)
    parser.parse()
    assert parser.num_minembhs == 0


def test_nonexistent_file(tmp_path):
    """Nonexistent file raises FileNotFoundError."""
    parser = MinembhParser(tmp_path / "nonexistent.dat")
    with pytest.raises(FileNotFoundError):
        parser.parse()
