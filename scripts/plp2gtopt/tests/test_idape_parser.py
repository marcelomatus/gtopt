"""Tests for IdApeParser — plpidape.dat parsing."""

from pathlib import Path
import textwrap

import pytest

from plp2gtopt.idape_parser import IdApeParser


@pytest.fixture()
def idape_file(tmp_path: Path) -> Path:
    """Create a minimal plpidape.dat test file (2 simulations, 3 stages)."""
    content = textwrap.dedent("""\
        # Archivo de caudales por etapa
        # Numero de simulaciones y etapas con caudales
              2       3
        # Mes   Etapa  NApert ApertInd(1,...,NApert) - Simulacion=01
           003   001    3   51   52   53
           004   002    3   51   52   53
           005   003    2   51   52
        # Mes   Etapa  NApert ApertInd(1,...,NApert) - Simulacion=02
           003   001    3   54   55   56
           004   002    3   54   55   56
           005   003    2   54   55
    """)
    p = tmp_path / "plpidape.dat"
    p.write_text(content)
    return p


def test_parse_basic(idape_file: Path) -> None:
    """Parse a basic plpidape.dat and check structure."""
    parser = IdApeParser(idape_file)
    parser.parse()
    assert parser.num_simulations == 2
    assert parser.num_stages == 3
    # 2 simulations × 3 stages = 6 entries
    assert len(parser.items) == 6


def test_get_apertures(idape_file: Path) -> None:
    """Check aperture retrieval for specific (simulation, stage) pairs."""
    parser = IdApeParser(idape_file)
    parser.parse()
    # simulation=0, stage=1 → [51, 52, 53]
    assert parser.get_apertures(0, 1) == [51, 52, 53]
    # simulation=0, stage=3 → [51, 52] (only 2 apertures)
    assert parser.get_apertures(0, 3) == [51, 52]
    # simulation=1, stage=2 → [54, 55, 56]
    assert parser.get_apertures(1, 2) == [54, 55, 56]


def test_get_apertures_not_found(idape_file: Path) -> None:
    """Non-existent (sim, stage) returns empty list."""
    parser = IdApeParser(idape_file)
    parser.parse()
    assert parser.get_apertures(99, 1) == []
    assert parser.get_apertures(0, 99) == []


def test_empty_file(tmp_path: Path) -> None:
    """Empty file results in no data."""
    p = tmp_path / "plpidape.dat"
    p.write_text("# empty\n")
    parser = IdApeParser(p)
    parser.parse()
    assert parser.num_simulations == 0
    assert len(parser.items) == 0


def test_real_case_structure(tmp_path: Path) -> None:
    """Test parsing with real-world-like data (16 sims, 51 stages, first sim only)."""
    lines = [
        "# Archivo de caudales por etapa",
        "# Numero de simulaciones y etapas con caudales",
        "      16      51",
    ]
    # Only write 2 stages for first simulation to test structure
    lines.append("# Mes   Etapa  NApert ApertInd(1,...,NApert) - Simulacion=01")
    lines.append("   003   001    16   " + "   ".join(str(i) for i in range(51, 67)))
    lines.append("   003   002    16   " + "   ".join(str(i) for i in range(51, 67)))
    p = tmp_path / "plpidape.dat"
    p.write_text("\n".join(lines) + "\n")
    parser = IdApeParser(p)
    parser.parse()
    assert parser.num_simulations == 16
    assert parser.num_stages == 51
    # Only 2 entries parsed (incomplete file but parser handles gracefully)
    assert len(parser.items) == 2
    assert parser.get_apertures(0, 1) == list(range(51, 67))
