"""Tests for aperture_writer — building aperture_array from PLP data."""

from pathlib import Path
import textwrap

import pytest

from plp2gtopt.idap2_parser import IdAp2Parser
from plp2gtopt.aperture_writer import build_aperture_array, build_phase_aperture_sets


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
    res = build_aperture_array(idap2_parser, scenario_hydro_map, 3, max_scenario_uid=4)
    result = res.aperture_array

    assert len(result) == 4
    # Each aperture's source_scenario should reference a forward scenario UID
    assert result[0]["source_scenario"] == 1  # hydro 51 → uid 1
    assert result[1]["source_scenario"] == 2  # hydro 52 → uid 2
    assert result[2]["source_scenario"] == 3  # hydro 53 → uid 3
    assert result[3]["source_scenario"] == 4  # hydro 54 → uid 4
    # Equal probability
    assert pytest.approx(result[0]["probability_factor"]) == 0.25
    # No extra scenarios needed — all in forward set
    assert res.extra_scenarios == []


def test_build_aperture_array_extra_hydros(idap2_parser: IdAp2Parser) -> None:
    """Some aperture hydros are NOT in the forward set."""
    # Only hydros 50,51 (0-based) are in forward set → 52,53 are aperture-only
    scenario_hydro_map = {50: 1, 51: 2}
    res = build_aperture_array(idap2_parser, scenario_hydro_map, 3, max_scenario_uid=2)
    result = res.aperture_array

    assert len(result) == 4
    assert result[0]["source_scenario"] == 1  # hydro 51 (0-based 50) → uid 1
    assert result[1]["source_scenario"] == 2  # hydro 52 (0-based 51) → uid 2
    # hydro 53 (0-based 52) not in forward → Fortran 1-based = 53
    assert result[2]["source_scenario"] == 53
    # hydro 54 (0-based 53) not in forward → Fortran 1-based = 54
    assert result[3]["source_scenario"] == 54

    # Extra scenarios created for aperture-only hydros
    assert len(res.extra_scenarios) == 2
    assert res.extra_scenarios[0]["uid"] == 53
    assert res.extra_scenarios[0]["hydrology"] == 52
    assert res.extra_scenarios[1]["uid"] == 54
    assert res.extra_scenarios[1]["hydrology"] == 53


def test_build_aperture_array_no_parser() -> None:
    """No parser → empty array."""
    res = build_aperture_array(None, {}, 3)
    assert not res.aperture_array


def test_build_aperture_array_empty_parser(tmp_path: Path) -> None:
    """Parser with no data → empty array."""
    p = tmp_path / "plpidap2.dat"
    p.write_text("# empty\n")
    parser = IdAp2Parser(p)
    parser.parse()
    res = build_aperture_array(parser, {}, 3)
    assert not res.aperture_array


def test_aperture_uids_are_sequential(idap2_parser: IdAp2Parser) -> None:
    """Aperture UIDs must be sequential starting from 1."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    res = build_aperture_array(idap2_parser, scenario_hydro_map, 3, max_scenario_uid=4)
    uids = [a["uid"] for a in res.aperture_array]
    assert uids == [1, 2, 3, 4]


def test_aperture_probabilities_sum_to_one(idap2_parser: IdAp2Parser) -> None:
    """Aperture probabilities must sum to 1."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    res = build_aperture_array(idap2_parser, scenario_hydro_map, 3, max_scenario_uid=4)
    total = sum(a["probability_factor"] for a in res.aperture_array)
    assert pytest.approx(total) == 1.0


# ---------------------------------------------------------------------------
# Tests for build_phase_aperture_sets
# ---------------------------------------------------------------------------


@pytest.fixture()
def idap2_varying(tmp_path: Path) -> IdAp2Parser:
    """Create and parse a plpidap2.dat with *different* apertures per stage.

    Stage 1: apertures [51, 52]
    Stage 2: apertures [51, 52, 53]
    Stage 3: apertures [1, 51, 52, 53]
    """
    content = textwrap.dedent("""\
        # Archivo de caudales
        # Numero de etapas
              3
        # Mes  Etapa  NApert  ApertInd
           001   001    2   51   52
           002   002    3   51   52   53
           003   003    4    1   51   52   53
    """)
    p = tmp_path / "plpidap2.dat"
    p.write_text(content)
    parser = IdAp2Parser(p)
    parser.parse()
    return parser


def test_phase_aperture_sets_uniform(idap2_parser: IdAp2Parser) -> None:
    """When all stages share the same apertures, no aperture_set is added."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    aperture_array = build_aperture_array(idap2_parser, scenario_hydro_map, 3).aperture_array
    # SDDP: one phase per stage
    phase_array = [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1},
        {"uid": 3, "first_stage": 2, "count_stage": 1},
    ]
    build_phase_aperture_sets(idap2_parser, aperture_array, phase_array, 3)
    # All phases use the same apertures [51,52,53,54] → no aperture_set added
    for phase in phase_array:
        assert "aperture_set" not in phase


def test_phase_aperture_sets_varying(idap2_varying: IdAp2Parser) -> None:
    """When stages have different apertures, per-phase aperture_set is added."""
    scenario_hydro_map = {0: 10, 50: 1, 51: 2, 52: 3}
    aperture_array = build_aperture_array(idap2_varying, scenario_hydro_map, 3).aperture_array
    # SDDP: one phase per stage
    phase_array = [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1},
        {"uid": 3, "first_stage": 2, "count_stage": 1},
    ]
    build_phase_aperture_sets(idap2_varying, aperture_array, phase_array, 3)

    # Stage 1 uses [51,52] → aperture UIDs for hydros 51,52
    # Stage 2 uses [51,52,53] → aperture UIDs for hydros 51,52,53
    # Stage 3 uses [1,51,52,53] → aperture UIDs for hydros 1,51,52,53
    # Each phase should have its own aperture_set
    assert "aperture_set" in phase_array[0]
    assert "aperture_set" in phase_array[1]
    assert "aperture_set" in phase_array[2]

    # Phase 3 has the most apertures (all), phase 1 has the fewest
    assert len(phase_array[0]["aperture_set"]) < len(phase_array[2]["aperture_set"])


def test_phase_aperture_sets_no_parser() -> None:
    """No parser → no modification."""
    phase_array = [{"uid": 1, "first_stage": 0, "count_stage": 1}]
    build_phase_aperture_sets(None, [], phase_array, 1)
    assert "aperture_set" not in phase_array[0]


def test_phase_aperture_sets_empty_inputs() -> None:
    """Empty aperture_array or phase_array → no modification."""
    build_phase_aperture_sets(None, [], [], 0)
    phase_array: list = [{"uid": 1, "first_stage": 0, "count_stage": 1}]
    build_phase_aperture_sets(None, [{"uid": 1}], phase_array, 1)
    assert "aperture_set" not in phase_array[0]


def test_phase_aperture_sets_multistage_duplicates(
    idap2_varying: IdAp2Parser,
) -> None:
    """Multi-stage phase preserves duplicate aperture UIDs.

    Stage 1 has apertures [51,52] and Stage 2 has [51,52,53].
    A phase spanning both stages should include duplicates for the
    apertures that appear in both stages (51 and 52), so the C++
    solver can weight them correctly by their repetition count.
    """
    scenario_hydro_map = {0: 10, 50: 1, 51: 2, 52: 3}
    aperture_array = build_aperture_array(idap2_varying, scenario_hydro_map, 3).aperture_array
    # Phase 1 spans stages 1+2, Phase 2 covers stage 3 only
    phase_array = [
        {"uid": 1, "first_stage": 0, "count_stage": 2},
        {"uid": 2, "first_stage": 2, "count_stage": 1},
    ]
    build_phase_aperture_sets(idap2_varying, aperture_array, phase_array, 3)

    # Phase 1 (stages 1+2): stage 1 → [51,52], stage 2 → [51,52,53]
    # With extend: hydros = [51,52,51,52,53] → sorted UIDs with duplicates
    ap_set_1 = phase_array[0]["aperture_set"]
    from collections import Counter

    counts_1 = Counter(ap_set_1)
    # hydro 51 → UID 1 appears 2× (stage 1 + stage 2)
    # hydro 52 → UID 2 appears 2× (stage 1 + stage 2)
    # hydro 53 → UID 3 appears 1× (stage 2 only)
    assert counts_1[1] == 2
    assert counts_1[2] == 2
    assert counts_1[3] == 1

    # Phase 2 (stage 3): [1,51,52,53] → 4 unique aperture UIDs, no duplicates
    ap_set_2 = phase_array[1]["aperture_set"]
    assert len(ap_set_2) == len(set(ap_set_2))
