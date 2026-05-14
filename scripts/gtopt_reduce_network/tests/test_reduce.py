# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end driver tests for ``reduce_case``."""

from __future__ import annotations

from pathlib import Path

import pytest

from gtopt_reduce_network._io import Case, load_case
from gtopt_reduce_network._reduce import ReduceConfig, reduce_case


def test_reduce_ieee14(ieee14_path: Path) -> None:
    case = load_case(ieee14_path)
    result = reduce_case(case, ReduceConfig(target_buses=7))
    n_buses = len(result.case.array("bus_array"))
    n_lines = len(result.case.array("line_array"))
    # Bus count must not exceed K + n_anchors-collisions (~+1 typical).
    assert n_buses <= 8
    assert n_buses >= 5
    # Reduced line count strictly fewer than original 20.
    assert n_lines < 20
    # Every original bus is mapped.
    assert {r.original_bus_uid for r in result.busmap} == set(case.bus_name_by_uid)
    # Every aggregator bus rewrite respects busmap.
    cluster_of = {r.original_bus_uid: r.cluster_bus_uid for r in result.busmap}
    for ar in result.aggregator:
        assert ar.cluster_bus_uid == cluster_of[ar.original_bus_uid]
        assert ar.share == pytest.approx(1.0)


def test_reduce_preserves_capacity_sum(ieee14_path: Path) -> None:
    """Sum of generator capacities must be preserved (units are kept)."""
    case = load_case(ieee14_path)
    orig_cap = sum(g["capacity"] for g in case.array("generator_array"))
    result = reduce_case(case, ReduceConfig(target_buses=7))
    new_cap = sum(g["capacity"] for g in result.case.array("generator_array"))
    assert new_cap == pytest.approx(orig_cap)


def test_reduce_K_equals_n_returns_original_topology(tiny_chain_case: Case) -> None:
    n = len(tiny_chain_case.array("bus_array"))
    result = reduce_case(
        tiny_chain_case, ReduceConfig(target_buses=n, skip_local_simplify=True)
    )
    assert len(result.case.array("bus_array")) == n


def test_reduce_with_local_simplify_eliminates_b3(tiny_chain_case: Case) -> None:
    """b3 has no injection and degree 2 (after parallel merge) → eliminated."""
    result = reduce_case(tiny_chain_case, ReduceConfig(target_buses=3))
    assert 3 in result.eliminated_buses or len(result.case.array("bus_array")) <= 3


def test_reduce_anchor_pinned(two_clusters_case: Case) -> None:
    cfg = ReduceConfig(target_buses=2, user_anchor_uids=(1, 6))
    result = reduce_case(two_clusters_case, cfg)
    cluster_of = {r.original_bus_uid: r.cluster_bus_uid for r in result.busmap}
    assert cluster_of[1] != cluster_of[6]


def test_reduce_unknown_distance_raises(ieee14_path: Path) -> None:
    case = load_case(ieee14_path)
    with pytest.raises(ValueError, match="unknown distance metric"):
        reduce_case(case, ReduceConfig(target_buses=7, distance="bogus"))


def test_reduce_writes_aggregator_for_every_referenced_element(
    ieee14_path: Path,
) -> None:
    case = load_case(ieee14_path)
    n_gen = len(case.array("generator_array"))
    n_dem = len(case.array("demand_array"))
    result = reduce_case(case, ReduceConfig(target_buses=7))
    kinds = [r.component_kind for r in result.aggregator]
    assert kinds.count("generator") == n_gen
    assert kinds.count("demand") == n_dem
