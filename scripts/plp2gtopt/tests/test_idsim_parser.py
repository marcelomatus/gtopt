"""Tests for IdSimParser — plpidsim.dat parsing."""

from pathlib import Path
import textwrap

import pytest

from plp2gtopt.idsim_parser import IdSimParser


@pytest.fixture()
def idsim_file(tmp_path: Path) -> Path:
    """Create a minimal plpidsim.dat test file."""
    content = textwrap.dedent("""\
        # Archivo de caudales por etapa
        # Numero de simulaciones y etapas con caudales
              3       4
        # Mes   Etapa  SimulInd(1,...,NSimul)
           003   001   51   52   53
           004   002   51   52   53
           005   003   52   53   54
           006   004   53   54   55
    """)
    p = tmp_path / "plpidsim.dat"
    p.write_text(content)
    return p


def test_parse_basic(idsim_file: Path) -> None:
    """Parse a basic plpidsim.dat and check structure."""
    parser = IdSimParser(idsim_file)
    parser.parse()
    assert parser.num_simulations == 3
    assert parser.num_stages == 4
    assert len(parser.items) == 4


def test_indices(idsim_file: Path) -> None:
    """Check individual index lookup."""
    parser = IdSimParser(idsim_file)
    parser.parse()
    # simulation=0, stage=1 → 51
    assert parser.get_index(0, 1) == 51
    # simulation=2, stage=3 → 54
    assert parser.get_index(2, 3) == 54
    # simulation=1, stage=4 → 54
    assert parser.get_index(1, 4) == 54


def test_out_of_range(idsim_file: Path) -> None:
    """Out-of-range queries return None."""
    parser = IdSimParser(idsim_file)
    parser.parse()
    assert parser.get_index(99, 1) is None
    assert parser.get_index(0, 99) is None


def test_empty_file(tmp_path: Path) -> None:
    """Empty file results in no data."""
    p = tmp_path / "plpidsim.dat"
    p.write_text("# empty\n")
    parser = IdSimParser(p)
    parser.parse()
    assert parser.num_simulations == 0
    assert len(parser.items) == 0


def test_get_stage_map(idsim_file: Path) -> None:
    """Full per-stage SimulInd row for one simulation (PLP rotation)."""
    parser = IdSimParser(idsim_file)
    parser.parse()
    # Simulation 0 advances 51 → 51 → 52 → 53 across the January boundary
    assert parser.get_stage_map(0) == {1: 51, 2: 51, 3: 52, 4: 53}
    assert parser.get_stage_map(2) == {1: 53, 2: 53, 3: 54, 4: 55}
    # Out-of-range simulation → empty map
    assert not parser.get_stage_map(99)


def test_has_rotation(idsim_file: Path) -> None:
    """Rotation is detected when any stage row differs from stage 1."""
    parser = IdSimParser(idsim_file)
    parser.parse()
    assert parser.has_rotation() is True
    assert parser.has_rotation(num_stages=4) is True
    # Stages 1-2 share the same row → no rotation inside that window
    assert parser.has_rotation(num_stages=2) is False
    assert parser.has_rotation(num_stages=1) is False


def test_has_rotation_static(tmp_path: Path) -> None:
    """A stage-constant mapping reports no rotation."""
    content = textwrap.dedent("""\
        # static idsim
        # NSimul NEtaCau
              2       3
        # Mes   Etapa  SimulInd
           001   001   51   52
           002   002   51   52
           003   003   51   52
    """)
    p = tmp_path / "plpidsim.dat"
    p.write_text(content)
    parser = IdSimParser(p)
    parser.parse()
    assert parser.has_rotation() is False
