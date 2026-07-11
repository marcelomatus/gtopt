"""Tests for aperture_writer — building aperture_array from PLP data."""

from pathlib import Path
import textwrap
from types import SimpleNamespace

import numpy as np
import pytest

from plp2gtopt.idap2_parser import IdAp2Parser
from plp2gtopt.aperture_writer import (
    build_aperture_array,
    build_phase_apertures,
    compute_global_wetness,
    compute_phase_wetness,
)


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


def test_build_aperture_array_extra_hydros_with_directory(
    idap2_parser: IdAp2Parser,
) -> None:
    """Aperture-only hydros are emitted WHEN ``aperture_directory`` is set.

    The aperture solver loads per-hydrology affluent data from this
    directory (populated by ``write_aperture_afluents``), so extra
    apertures referencing scenario UIDs that won't be in
    ``scenario_array`` are still resolvable at runtime.
    """
    # Only hydros 50,51 (0-based) are in forward set → 52,53 are aperture-only
    scenario_hydro_map = {50: 1, 51: 2}
    res = build_aperture_array(
        idap2_parser,
        scenario_hydro_map,
        3,
        max_scenario_uid=2,
        aperture_directory="some/path/to/apertures",
    )
    result = res.aperture_array

    assert len(result) == 4
    assert result[0]["source_scenario"] == 1  # hydro 51 (0-based 50) → uid 1
    assert result[1]["source_scenario"] == 2  # hydro 52 (0-based 51) → uid 2
    # hydro 53 (0-based 52) not in forward → Fortran 1-based = 53
    assert result[2]["source_scenario"] == 53
    # hydro 54 (0-based 53) not in forward → Fortran 1-based = 54
    assert result[3]["source_scenario"] == 54

    # Equal probability across all 4 surviving apertures.
    assert pytest.approx(result[0]["probability_factor"]) == 0.25
    assert pytest.approx(result[3]["probability_factor"]) == 0.25

    # Extra scenarios created for aperture-only hydros
    assert len(res.extra_scenarios) == 2
    assert res.extra_scenarios[0]["uid"] == 53
    assert res.extra_scenarios[0]["hydrology"] == 52
    assert res.extra_scenarios[1]["uid"] == 54
    assert res.extra_scenarios[1]["hydrology"] == 53


def test_build_aperture_array_extra_hydros_no_directory(
    idap2_parser: IdAp2Parser,
) -> None:
    """Aperture-only hydros are DROPPED when ``aperture_directory`` is unset.

    Without an aperture_directory the C++ aperture solver has nowhere
    to load the per-hydrology affluent data from — at runtime it logs
    "source_scenario X not found" and falls back to a plain Benders
    cut.  Those silently-dropped apertures pollute every phase's
    aperture list and now (per
    ``check_aperture_references`` in validate_planning) would surface
    as a hard validation error since the source_scenario UIDs aren't
    in scenario_array.  Drop the aperture-only entries entirely
    instead — the Benders cut path produces equivalent results
    without the misleading warnings.
    """
    scenario_hydro_map = {50: 1, 51: 2}
    res = build_aperture_array(
        idap2_parser,
        scenario_hydro_map,
        3,
        max_scenario_uid=2,
        # aperture_directory left at default ""
    )
    result = res.aperture_array

    # Only the 2 forward-set hydros survive.
    assert len(result) == 2
    assert result[0]["source_scenario"] == 1
    assert result[1]["source_scenario"] == 2
    # Probability re-normalised to 1/2 across the surviving apertures.
    assert pytest.approx(result[0]["probability_factor"]) == 0.5
    assert pytest.approx(result[1]["probability_factor"]) == 0.5
    # No extra scenarios since no aperture-only entries were emitted.
    assert res.extra_scenarios == []


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


def test_aperture_uids_match_fortran_hydrology(idap2_parser: IdAp2Parser) -> None:
    """Aperture UIDs must match the 1-based PLP hydrology number."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    res = build_aperture_array(idap2_parser, scenario_hydro_map, 3, max_scenario_uid=4)
    uids = [a["uid"] for a in res.aperture_array]
    # hydros (1-based) 51,52,53,54 → aperture UIDs 51,52,53,54
    assert uids == [51, 52, 53, 54]


def test_aperture_probabilities_sum_to_one(idap2_parser: IdAp2Parser) -> None:
    """Aperture probabilities must sum to 1."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    res = build_aperture_array(idap2_parser, scenario_hydro_map, 3, max_scenario_uid=4)
    total = sum(a["probability_factor"] for a in res.aperture_array)
    assert pytest.approx(total) == 1.0


# ---------------------------------------------------------------------------
# Tests for build_phase_apertures
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


def test_phase_aperturess_uniform(idap2_parser: IdAp2Parser) -> None:
    """When all stages share the same apertures, no apertures is added."""
    scenario_hydro_map = {50: 1, 51: 2, 52: 3, 53: 4}
    aperture_array = build_aperture_array(
        idap2_parser, scenario_hydro_map, 3
    ).aperture_array
    # SDDP: one phase per stage
    phase_array = [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1},
        {"uid": 3, "first_stage": 2, "count_stage": 1},
    ]
    build_phase_apertures(idap2_parser, aperture_array, phase_array, 3)
    # All phases use the same apertures [51,52,53,54] → no apertures added
    for phase in phase_array:
        assert "apertures" not in phase


def test_phase_aperturess_varying(idap2_varying: IdAp2Parser) -> None:
    """When stages have different apertures, per-phase apertures is added."""
    scenario_hydro_map = {0: 10, 50: 1, 51: 2, 52: 3}
    aperture_array = build_aperture_array(
        idap2_varying, scenario_hydro_map, 3
    ).aperture_array
    # SDDP: one phase per stage
    phase_array = [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1},
        {"uid": 3, "first_stage": 2, "count_stage": 1},
    ]
    build_phase_apertures(idap2_varying, aperture_array, phase_array, 3)

    # Stage 1 uses [51,52] → aperture UIDs for hydros 51,52
    # Stage 2 uses [51,52,53] → aperture UIDs for hydros 51,52,53
    # Stage 3 uses [1,51,52,53] → aperture UIDs for hydros 1,51,52,53
    # Each phase should have its own apertures
    assert "apertures" in phase_array[0]
    assert "apertures" in phase_array[1]
    assert "apertures" in phase_array[2]

    # Phase 3 has the most apertures (all), phase 1 has the fewest
    assert len(phase_array[0]["apertures"]) < len(phase_array[2]["apertures"])


def test_phase_aperturess_no_parser() -> None:
    """No parser → no modification."""
    phase_array = [{"uid": 1, "first_stage": 0, "count_stage": 1}]
    build_phase_apertures(None, [], phase_array, 1)
    assert "apertures" not in phase_array[0]


def test_phase_aperturess_empty_inputs() -> None:
    """Empty aperture_array or phase_array → no modification."""
    build_phase_apertures(None, [], [], 0)
    phase_array: list = [{"uid": 1, "first_stage": 0, "count_stage": 1}]
    build_phase_apertures(None, [{"uid": 1}], phase_array, 1)
    assert "apertures" not in phase_array[0]


def test_phase_aperturess_multistage_duplicates(
    idap2_varying: IdAp2Parser,
) -> None:
    """Multi-stage phase preserves duplicate aperture UIDs.

    Stage 1 has apertures [51,52] and Stage 2 has [51,52,53].
    A phase spanning both stages should include duplicates for the
    apertures that appear in both stages (51 and 52), so the C++
    solver can weight them correctly by their repetition count.
    """
    scenario_hydro_map = {0: 10, 50: 1, 51: 2, 52: 3}
    aperture_array = build_aperture_array(
        idap2_varying, scenario_hydro_map, 3
    ).aperture_array
    # Phase 1 spans stages 1+2, Phase 2 covers stage 3 only
    phase_array = [
        {"uid": 1, "first_stage": 0, "count_stage": 2},
        {"uid": 2, "first_stage": 2, "count_stage": 1},
    ]
    build_phase_apertures(idap2_varying, aperture_array, phase_array, 3)

    # Phase 1 (stages 1+2): stage 1 → [51,52], stage 2 → [51,52,53]
    # With extend: hydros = [51,52,51,52,53] → sorted UIDs with duplicates
    ap_set_1 = phase_array[0]["apertures"]
    from collections import Counter

    counts_1 = Counter(ap_set_1)
    # Aperture UID = 1-based hydrology number (PLP Fortran convention):
    # hydro 51 → UID 51 appears 2× (stage 1 + stage 2)
    # hydro 52 → UID 52 appears 2× (stage 1 + stage 2)
    # hydro 53 → UID 53 appears 1× (stage 2 only)
    assert counts_1[51] == 2
    assert counts_1[52] == 2
    assert counts_1[53] == 1

    # Phase 2 (stage 3): [1,51,52,53] → 4 unique aperture UIDs, no duplicates
    ap_set_2 = phase_array[1]["apertures"]
    assert len(ap_set_2) == len(set(ap_set_2))


# ---------------------------------------------------------------------------
# Tests for the wetness sort (driest → wettest aperture ordering)
# ---------------------------------------------------------------------------


def _make_aflce(centrals):
    """Build a SimpleNamespace mimicking AflceParser.items.

    Each ``centrals`` entry is ``(name, block_numbers, flow_matrix)``.
    """
    items = []
    for name, block_nums, flow in centrals:
        items.append(
            {
                "name": name,
                "block": np.asarray(block_nums, dtype=np.int32),
                "flow": np.asarray(flow, dtype=np.float64),
            }
        )
    return SimpleNamespace(items=items)


def _make_block_parser(blocks):
    """Build a SimpleNamespace mimicking BlockParser.items.

    Each ``blocks`` entry is ``(number, stage, duration)``.
    """
    items = [
        {"number": int(n), "stage": int(s), "duration": float(d)}
        for (n, s, d) in blocks
    ]
    return SimpleNamespace(items=items)


def _make_idap2_uniform(tmp_path: Path, hydros, num_stages=3) -> IdAp2Parser:
    """Single-stage-per-line idap2 file where every stage references ``hydros``."""
    header = textwrap.dedent("""\
        # Archivo de caudales
        # Numero de etapas
              {ns}
        # Mes  Etapa  NApert  ApertInd
    """).format(ns=num_stages)
    rows = []
    for s in range(1, num_stages + 1):
        # Mes column doesn't matter for parsing logic here; reuse stage value.
        idx_str = "  ".join(f"{h:>3}" for h in hydros)
        rows.append(f"   {s:03d}   {s:03d}   {len(hydros):2d}  {idx_str}")
    p = tmp_path / "plpidap2.dat"
    p.write_text(header + "\n".join(rows) + "\n")
    parser = IdAp2Parser(p)
    parser.parse()
    return parser


def test_wetness_sort_wettest_first(tmp_path: Path) -> None:
    """3 hydros with known totals sort wettest → driest (uid tiebreak unused)."""
    # Two blocks: durations [1, 2].  One central with flows:
    #   h0 (uid 1) = [10, 10] → total = 10*1 + 10*2 = 30
    #   h1 (uid 2) = [ 1,  1] → total =  1*1 +  1*2 =  3
    #   h2 (uid 3) = [ 5,  5] → total =  5*1 +  5*2 = 15
    # Expected order (wettest → driest): [1, 3, 2]
    aflce = _make_aflce([("c1", [1, 2], [[10.0, 1.0, 5.0], [10.0, 1.0, 5.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0), (2, 1, 2.0)])
    idap2 = _make_idap2_uniform(tmp_path, hydros=[1, 2, 3], num_stages=1)
    scenario_hydro_map = {0: 1, 1: 2, 2: 3}
    aperture_array = build_aperture_array(
        idap2,
        scenario_hydro_map,
        num_stages=1,
        aflce_parser=aflce,
        block_parser=block_parser,
    ).aperture_array
    # With aflce/block parsers, aperture_array is sorted by global
    # wetness desc — for a 1-stage case that equals phase wetness.
    assert [a["uid"] for a in aperture_array] == [1, 3, 2]
    phase_array = [{"uid": 1, "first_stage": 0, "count_stage": 1}]
    build_phase_apertures(
        idap2,
        aperture_array,
        phase_array,
        1,
        aflce_parser=aflce,
        block_parser=block_parser,
    )
    w = compute_phase_wetness(aflce, block_parser, phase_array[0], 1)
    assert w[1] == pytest.approx(30.0)
    assert w[2] == pytest.approx(3.0)
    assert w[3] == pytest.approx(15.0)
    # Confirm sort key produces the expected ordering (wettest first).
    ordered = sorted([1, 2, 3], key=lambda uid: (-w.get(uid, 0.0), uid))
    assert ordered == [1, 3, 2]


def test_wetness_sort_stable_tiebreak_by_uid(tmp_path: Path) -> None:
    """Equal wetness → ascending UID (tiebreak under wettest-first sort)."""
    # Two hydros with identical flows → identical totals.  Tiebreak on uid.
    aflce = _make_aflce([("c1", [1], [[7.0, 7.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0)])
    w = compute_phase_wetness(
        aflce,
        block_parser,
        {"first_stage": 0, "count_stage": 1},
        num_stages=1,
    )
    assert w[1] == w[2]
    # Wettest-first sort with uid-asc tiebreak: equal wetness ⇒ uid asc.
    ordered = sorted([2, 1], key=lambda uid: (-w.get(uid, 0.0), uid))
    assert ordered == [1, 2]


def test_wetness_sort_duplicates_adjacent(tmp_path: Path) -> None:
    """Stable sort places duplicate UIDs adjacent (no interleaving)."""
    # h1 (uid 1) wetness 100, h2 (uid 2) wetness 1.
    aflce = _make_aflce([("c1", [1], [[100.0, 1.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0)])
    w = compute_phase_wetness(
        aflce,
        block_parser,
        {"first_stage": 0, "count_stage": 1},
        num_stages=1,
    )
    multiset = [1, 2, 1, 2, 2]  # mixed-order input with duplicates
    ordered = sorted(multiset, key=lambda uid: (-w.get(uid, 0.0), uid))
    # Wettest first (uid 1 ×2), then driest (uid 2 ×3), all adjacent.
    assert ordered == [1, 1, 2, 2, 2]


def test_wetness_sort_no_aflce_falls_back_to_uid(tmp_path: Path) -> None:
    """Missing aflce_parser → empty wetness → sort key reduces to (0, uid)."""
    w_none = compute_phase_wetness(
        None,
        None,
        {"first_stage": 0, "count_stage": 1},
        num_stages=1,
    )
    assert not w_none
    # With wettest-first key and empty wetness map: all wetness values are
    # 0, so the negation is also 0 → sort falls back to uid ascending.
    ordered = sorted([3, 1, 2], key=lambda uid: (-w_none.get(uid, 0.0), uid))
    assert ordered == [1, 2, 3]


def test_wetness_sort_nan_treated_as_zero(tmp_path: Path) -> None:
    """A single NaN flow value contributes 0; other hydros rank correctly."""
    aflce = _make_aflce([("c1", [1, 2], [[float("nan"), 4.0], [3.0, 5.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0), (2, 1, 1.0)])
    w = compute_phase_wetness(
        aflce,
        block_parser,
        {"first_stage": 0, "count_stage": 1},
        num_stages=1,
    )
    # h1 (uid 1): NaN → 0, plus 3 → total 3.
    # h2 (uid 2): 4 + 5 = 9.
    assert w[1] == pytest.approx(3.0)
    assert w[2] == pytest.approx(9.0)


def test_wetness_sort_per_phase_ranking_can_flip(tmp_path: Path) -> None:
    """Two phases with disjoint stage windows can rank hydros differently."""
    # Stage 1 block: h1 wet (10), h2 dry (1).
    # Stage 2 block: h1 dry (1), h2 wet (10).
    aflce = _make_aflce([("c1", [1, 2], [[10.0, 1.0], [1.0, 10.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0), (2, 2, 1.0)])
    phase_a = {"first_stage": 0, "count_stage": 1}  # PLP stage 1 only
    phase_b = {"first_stage": 1, "count_stage": 1}  # PLP stage 2 only
    w_a = compute_phase_wetness(aflce, block_parser, phase_a, num_stages=2)
    w_b = compute_phase_wetness(aflce, block_parser, phase_b, num_stages=2)
    # Phase A: h1 wetter than h2.  Phase B: flipped.
    assert w_a[1] > w_a[2]
    assert w_b[2] > w_b[1]
    # Wettest-first sort: phase A puts h1 first; phase B puts h2 first.
    assert sorted([1, 2], key=lambda u: (-w_a.get(u, 0.0), u)) == [1, 2]
    assert sorted([1, 2], key=lambda u: (-w_b.get(u, 0.0), u)) == [2, 1]


# ----------------------------------------------------------------------------
# Tests for the global wetness sort applied to aperture_array
# ----------------------------------------------------------------------------


def test_global_wetness_orders_aperture_array(tmp_path: Path) -> None:
    """aperture_array entries are ordered wettest → driest when affluent
    data is wired through."""
    # Same fixture as test_wetness_sort_wettest_first but verified at the
    # aperture_array layer instead of the per-phase sort key.
    #   h0 (uid 1) = 30 (wettest)
    #   h1 (uid 2) = 3  (driest)
    #   h2 (uid 3) = 15
    aflce = _make_aflce([("c1", [1, 2], [[10.0, 1.0, 5.0], [10.0, 1.0, 5.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0), (2, 1, 2.0)])
    idap2 = _make_idap2_uniform(tmp_path, hydros=[1, 2, 3], num_stages=1)
    scenario_hydro_map = {0: 1, 1: 2, 2: 3}
    res = build_aperture_array(
        idap2,
        scenario_hydro_map,
        num_stages=1,
        aflce_parser=aflce,
        block_parser=block_parser,
    )
    assert [a["uid"] for a in res.aperture_array] == [1, 3, 2]


def test_aperture_array_first_appearance_fallback_no_aflce(
    tmp_path: Path,
) -> None:
    """Without aflce/block parsers, aperture_array stays in first-appearance
    order from plpidap2.dat (legacy behaviour preserved)."""
    idap2 = _make_idap2_uniform(tmp_path, hydros=[3, 1, 2], num_stages=1)
    scenario_hydro_map = {0: 1, 1: 2, 2: 3}
    res = build_aperture_array(idap2, scenario_hydro_map, num_stages=1)
    assert [a["uid"] for a in res.aperture_array] == [3, 1, 2]


def test_compute_global_wetness_sums_all_stages(tmp_path: Path) -> None:
    """Global wetness aggregates flow × duration across the full horizon."""
    aflce = _make_aflce([("c1", [1, 2], [[10.0, 1.0], [1.0, 10.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0), (2, 2, 1.0)])
    w_global = compute_global_wetness(aflce, block_parser, num_stages=2)
    # Stage 1 contribs: h1=10, h2=1.  Stage 2 contribs: h1=1, h2=10.
    # Global totals: h1 = 11, h2 = 11.
    assert w_global[1] == pytest.approx(11.0)
    assert w_global[2] == pytest.approx(11.0)


def test_global_wetness_aggregates_across_multiple_centrals(tmp_path: Path) -> None:
    """``compute_global_wetness`` sums each hydro's flow×duration across
    every central, looking up duration by **block number** (not row index).

    Central B intentionally lists its blocks in reverse order ([2, 1]) so
    a buggy row-index lookup would attribute block 2's duration (3.0) to
    block 1's flow row — this test pins the by-number lookup invariant.
    """
    # Two blocks, both stage 1: durations 2.0 and 3.0.
    # Central A: blocks [1, 2], h1 contrib = 10*2 + 10*3 = 50;
    #                          h2 contrib =  1*2 +  1*3 =  5.
    # Central B: blocks [2, 1] (reversed!); first row pairs with block 2
    # (duration 3.0), second row with block 1 (duration 2.0):
    #   h1 contrib = 2*3 + 2*2 = 10; h2 contrib = 7*3 + 7*2 = 35.
    # Totals: w[1] = 50 + 10 = 60; w[2] = 5 + 35 = 40.
    aflce = _make_aflce(
        [
            ("A", [1, 2], [[10.0, 1.0], [10.0, 1.0]]),
            ("B", [2, 1], [[2.0, 7.0], [2.0, 7.0]]),
        ]
    )
    block_parser = _make_block_parser([(1, 1, 2.0), (2, 1, 3.0)])
    w = compute_global_wetness(aflce, block_parser, num_stages=1)
    assert w[1] == pytest.approx(60.0)
    assert w[2] == pytest.approx(40.0)


def test_compute_global_wetness_empty_without_parsers() -> None:
    """Missing parsers → empty dict (callers fall back to UID order)."""
    assert not compute_global_wetness(None, None, num_stages=3)


def test_compute_global_wetness_zero_stages() -> None:
    """num_stages <= 0 → empty dict."""
    aflce = _make_aflce([("c1", [1], [[1.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0)])
    assert not compute_global_wetness(aflce, block_parser, num_stages=0)


# ----------------------------------------------------------------------------
# Tests for the per-phase apertures emission rule (omit-when-matches-global)
# ----------------------------------------------------------------------------


def test_phase_apertures_emitted_when_order_differs_from_global(
    tmp_path: Path,
) -> None:
    """Two phases that re-rank hydros differently → both phases carry
    their own ``apertures`` field."""
    # Phase A: stage 1; h1 wet, h2 dry.
    # Phase B: stage 2; flipped — h1 dry, h2 wet.
    aflce = _make_aflce([("c1", [1, 2], [[10.0, 1.0], [1.0, 10.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0), (2, 2, 1.0)])
    idap2 = _make_idap2_uniform(tmp_path, hydros=[1, 2], num_stages=2)
    scenario_hydro_map = {0: 1, 1: 2}
    aperture_array = build_aperture_array(
        idap2,
        scenario_hydro_map,
        num_stages=2,
        aflce_parser=aflce,
        block_parser=block_parser,
    ).aperture_array
    phase_array = [
        {"uid": 1, "first_stage": 0, "count_stage": 1},
        {"uid": 2, "first_stage": 1, "count_stage": 1},
    ]
    build_phase_apertures(
        idap2,
        aperture_array,
        phase_array,
        2,
        aflce_parser=aflce,
        block_parser=block_parser,
    )
    # Phase A should rank [1, 2]; phase B should rank [2, 1] — different
    # from global ordering, so apertures must be emitted on every phase.
    assert "apertures" in phase_array[0]
    assert "apertures" in phase_array[1]
    assert phase_array[0]["apertures"] == [1, 2]
    assert phase_array[1]["apertures"] == [2, 1]


def test_phase_apertures_omitted_when_match_global_projection(
    tmp_path: Path,
) -> None:
    """When every phase's ordered list equals its projection onto the
    global aperture_array order, the per-phase field is omitted."""
    # Single stage → phase wetness equals global wetness → identical
    # ordering.  Hence no per-phase override is needed.
    aflce = _make_aflce([("c1", [1], [[10.0, 1.0, 5.0]])])
    block_parser = _make_block_parser([(1, 1, 1.0)])
    idap2 = _make_idap2_uniform(tmp_path, hydros=[1, 2, 3], num_stages=1)
    scenario_hydro_map = {0: 1, 1: 2, 2: 3}
    aperture_array = build_aperture_array(
        idap2,
        scenario_hydro_map,
        num_stages=1,
        aflce_parser=aflce,
        block_parser=block_parser,
    ).aperture_array
    phase_array = [{"uid": 1, "first_stage": 0, "count_stage": 1}]
    build_phase_apertures(
        idap2,
        aperture_array,
        phase_array,
        1,
        aflce_parser=aflce,
        block_parser=block_parser,
    )
    assert "apertures" not in phase_array[0]


def test_build_aperture_array_decoupled(idap2_parser: IdAp2Parser) -> None:
    """decouple_forward: no aperture aliases a forward scenario.

    With a rotating plpidsim.dat the forward scenarios' Flow data is
    stage-rotated, while plpidap2 aperture classes reference RAW aflce
    columns — so every aperture must be cache-backed under a
    collision-free offset UID (100 + hydro_1based).
    """
    scenario_hydro_map = {50: 51, 51: 52, 52: 53, 53: 54}
    res = build_aperture_array(
        idap2_parser,
        scenario_hydro_map,
        3,
        max_scenario_uid=54,
        aperture_directory="some/path/to/apertures",
        decouple_forward=True,
    )
    result = res.aperture_array

    assert len(result) == 4
    # Aperture uids keep the raw hydro class; sources are offset uids.
    assert [a["uid"] for a in result] == [51, 52, 53, 54]
    assert [a["source_scenario"] for a in result] == [151, 152, 153, 154]

    # EVERY aperture hydro gets a cache scenario entry (raw column).
    assert [e["uid"] for e in res.extra_scenarios] == [151, 152, 153, 154]
    assert [e["hydrology"] for e in res.extra_scenarios] == [50, 51, 52, 53]
    for e in res.extra_scenarios:
        assert e["input_directory"] == "some/path/to/apertures"


def test_build_aperture_array_decoupled_requires_directory(
    idap2_parser: IdAp2Parser,
) -> None:
    """decouple_forward without a directory falls back to aliasing."""
    scenario_hydro_map = {50: 51, 51: 52, 52: 53, 53: 54}
    res = build_aperture_array(
        idap2_parser,
        scenario_hydro_map,
        3,
        max_scenario_uid=54,
        aperture_directory="",
        decouple_forward=True,
    )
    # Fallback: forward aliasing preserved (legacy behaviour)
    assert [a["source_scenario"] for a in res.aperture_array] == [51, 52, 53, 54]
    assert res.extra_scenarios == []


def test_aperture_scenario_uid_offset() -> None:
    """Offset is the smallest power of ten above max_scenario_uid (min 100)."""
    from plp2gtopt.aperture_writer import aperture_scenario_uid_offset

    assert aperture_scenario_uid_offset(0) == 100
    assert aperture_scenario_uid_offset(68) == 100
    assert aperture_scenario_uid_offset(99) == 100
    assert aperture_scenario_uid_offset(100) == 1000
    assert aperture_scenario_uid_offset(366) == 1000
    assert aperture_scenario_uid_offset(1000) == 10000
