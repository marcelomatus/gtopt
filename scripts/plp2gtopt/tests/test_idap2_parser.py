"""Tests for IdAp2Parser — plpidap2.dat parsing."""

from pathlib import Path
import textwrap

import pytest

from plp2gtopt.idap2_parser import IdAp2Parser


@pytest.fixture()
def idap2_file(tmp_path: Path) -> Path:
    """Create a minimal plpidap2.dat test file."""
    content = textwrap.dedent("""\
        # Archivo de caudales por etapa (plpidap2.dat)
        # Numero de etapas con caudales
              4
        # Mes   Etapa  NApert ApertInd(1,...,NApert)
           003   001    3   51   52   53
           004   002    3   51   52   53
           005   003    3   52   53   54
           006   004    3   53   54   55
    """)
    p = tmp_path / "plpidap2.dat"
    p.write_text(content)
    return p


def test_parse_basic(idap2_file: Path) -> None:
    """Parse a basic plpidap2.dat and check structure."""
    parser = IdAp2Parser(idap2_file)
    parser.parse()
    assert parser.num_stages == 4
    assert len(parser.items) == 4


def test_get_apertures(idap2_file: Path) -> None:
    """Check aperture retrieval by stage."""
    parser = IdAp2Parser(idap2_file)
    parser.parse()
    assert parser.get_apertures(1) == [51, 52, 53]
    assert parser.get_apertures(3) == [52, 53, 54]
    assert parser.get_apertures(4) == [53, 54, 55]


def test_get_apertures_not_found(idap2_file: Path) -> None:
    """Non-existent stage returns empty list."""
    parser = IdAp2Parser(idap2_file)
    parser.parse()
    assert parser.get_apertures(99) == []


def test_empty_file(tmp_path: Path) -> None:
    """Empty file results in no data."""
    p = tmp_path / "plpidap2.dat"
    p.write_text("# empty\n")
    parser = IdAp2Parser(p)
    parser.parse()
    assert parser.num_stages == 0
    assert len(parser.items) == 0


def test_real_case_16_apertures(tmp_path: Path) -> None:
    """Test with 16 apertures per stage, matching plp_case_2y format."""
    lines = [
        "# Archivo de caudales por etapa (plpidap2.dat)",
        "# Numero de etapas con caudales",
        "      3",
        "# Mes   Etapa  NApert ApertInd(1,...,NApert)",
    ]
    # 16 apertures per stage
    hydros = list(range(51, 67))  # 16 values
    lines.append("   003   001    16   " + "   ".join(f"{h:02d}" for h in hydros))
    lines.append("   004   002    16   " + "   ".join(f"{h:02d}" for h in hydros))
    # Stage 3: different apertures (shifted)
    hydros2 = list(range(52, 68))
    lines.append("   005   003    16   " + "   ".join(f"{h:02d}" for h in hydros2))
    p = tmp_path / "plpidap2.dat"
    p.write_text("\n".join(lines) + "\n")
    parser = IdAp2Parser(p)
    parser.parse()
    assert parser.num_stages == 3
    assert len(parser.items) == 3
    assert parser.get_apertures(1) == list(range(51, 67))
    assert parser.get_apertures(3) == list(range(52, 68))
