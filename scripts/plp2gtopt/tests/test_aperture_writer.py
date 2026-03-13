"""Tests for aperture_writer — building aperture_array from PLP data."""

from pathlib import Path
import textwrap

import pytest

from plp2gtopt.idap2_parser import IdAp2Parser
from plp2gtopt.aperture_writer import build_aperture_array


@pytest.fixture()
def idap2_parser(tmp_path: Path) -> IdAp2Parser:
    """Create and parse a minimal plpidap2.dat."""
    content = textwrap.dedent("""\
        # Archivo de caudales
        # Numero de etapas
              3
        # Mes  Etapa  NApert  ApertInd
           003   001    4   51   52   53   54
           004   002    4   51   52   53   54
           005   003    4   51   52   53   54
    """)
    p = tmp_path / "plpidap2.dat"
    p.write_text(content)
    parser = IdAp2Parser(p)
    parser.parse()
    return parser


def test_build_aperture_array_all_in_forward(idap2_parser: IdAp2Parser) -> None:
    """All aperture hydros are in the forward scenario set."""
    # Forward scenarios use hydros 50..53 (0-based) = 51..54 (1-based)
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    result = build_aperture_array(idap2_parser, scenario_hydro_map, 3)

    assert len(result) == 4
    # Each aperture's source_scenario should reference a forward scenario UID
    assert result[0]["source_scenario"] == 1  # hydro 51 → uid 1
    assert result[1]["source_scenario"] == 2  # hydro 52 → uid 2
    assert result[2]["source_scenario"] == 3  # hydro 53 → uid 3
    assert result[3]["source_scenario"] == 4  # hydro 54 → uid 4
    # Equal probability
    assert pytest.approx(result[0]["probability_factor"]) == 0.25


def test_build_aperture_array_extra_hydros(idap2_parser: IdAp2Parser) -> None:
    """Some aperture hydros are NOT in the forward set."""
    # Only hydros 50,51 (0-based) are in forward set → 52,53 are aperture-only
    scenario_hydro_map = {50: 1, 51: 2}
    result = build_aperture_array(idap2_parser, scenario_hydro_map, 3)

    assert len(result) == 4
    assert result[0]["source_scenario"] == 1  # hydro 51 (0-based 50) → uid 1
    assert result[1]["source_scenario"] == 2  # hydro 52 (0-based 51) → uid 2
    # hydro 53 (0-based 52) not in forward → uses hydro_1based=53 as UID
    assert result[2]["source_scenario"] == 53
    # hydro 54 (0-based 53) not in forward → uses hydro_1based=54 as UID
    assert result[3]["source_scenario"] == 54


def test_build_aperture_array_no_parser() -> None:
    """No parser → empty array."""
    result = build_aperture_array(None, {}, 3)
    assert not result


def test_build_aperture_array_empty_parser(tmp_path: Path) -> None:
    """Parser with no data → empty array."""
    p = tmp_path / "plpidap2.dat"
    p.write_text("# empty\n")
    parser = IdAp2Parser(p)
    parser.parse()
    result = build_aperture_array(parser, {}, 3)
    assert not result


def test_aperture_uids_are_sequential(idap2_parser: IdAp2Parser) -> None:
    """Aperture UIDs must be sequential starting from 1."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    result = build_aperture_array(idap2_parser, scenario_hydro_map, 3)
    uids = [a["uid"] for a in result]
    assert uids == [1, 2, 3, 4]


def test_aperture_probabilities_sum_to_one(idap2_parser: IdAp2Parser) -> None:
    """Aperture probabilities must sum to 1."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    result = build_aperture_array(idap2_parser, scenario_hydro_map, 3)
    total = sum(a["probability_factor"] for a in result)
    assert pytest.approx(total) == 1.0
