"""Tests for PlpmatParser — plpmat.dat parsing."""

from pathlib import Path
import textwrap

import pytest

from plp2gtopt.plpmat_parser import PlpmatParser


@pytest.fixture()
def plpmat_file(tmp_path: Path) -> Path:
    """Create a minimal plpmat.dat test file."""
    content = textwrap.dedent("""\
        # Archivo con Parametros Matematicos (plpmat.dat)
        # PDMaxIte    PDError UmbIntConf NPlanosPorDefecto
            200          0.001       0.001                50
        # PMMaxIte    PMError
            1            5.0
        # Lambda CTasa CCauFal  CVert CInter Ctransm FCotFinEF FPreProc FPrevia
            0.00   0.0  7000.0   0.01   0.01    0.01         F        F       F
        # FFixTrasm  FSeparaFCF  FGrabaCSV   FGrabaRES
          T          F           T           F
        # ABLMax   ABLEpsilon NumEtaCF
            20        0.001         1
        # FConvPGradx FConvPVar UmbGradX UmbVar
          F           F         0.5       100
    """)
    p = tmp_path / "plpmat.dat"
    p.write_text(content)
    return p


def test_parse_basic(plpmat_file: Path) -> None:
    """Parse a basic plpmat.dat and check max_iterations."""
    parser = PlpmatParser(plpmat_file)
    parser.parse()
    assert parser.max_iterations == 200


def test_parse_pd_error(plpmat_file: Path) -> None:
    """Check PDError is parsed correctly."""
    parser = PlpmatParser(plpmat_file)
    parser.parse()
    assert parser.pd_error == pytest.approx(0.001)


def test_parse_default_cuts(plpmat_file: Path) -> None:
    """Check NPlanosPorDefecto is parsed correctly."""
    parser = PlpmatParser(plpmat_file)
    parser.parse()
    assert parser.default_cuts == 50


def test_parse_flow_fail_cost(plpmat_file: Path) -> None:
    """Check CCauFal (hydro flow failure cost) is parsed from line 3."""
    parser = PlpmatParser(plpmat_file)
    parser.parse()
    assert parser.flow_fail_cost == pytest.approx(7000.0)


def test_empty_file(tmp_path: Path) -> None:
    """Empty file results in zero defaults."""
    p = tmp_path / "plpmat.dat"
    p.write_text("# empty\n")
    parser = PlpmatParser(p)
    parser.parse()
    assert parser.max_iterations == 0
    assert parser.pd_error == 0.0
    assert parser.default_cuts == 0


def test_single_field(tmp_path: Path) -> None:
    """File with only PDMaxIte on the data line."""
    content = textwrap.dedent("""\
        # comment
        100
    """)
    p = tmp_path / "plpmat.dat"
    p.write_text(content)
    parser = PlpmatParser(p)
    parser.parse()
    assert parser.max_iterations == 100
    assert parser.pd_error == 0.0
    assert parser.default_cuts == 0
