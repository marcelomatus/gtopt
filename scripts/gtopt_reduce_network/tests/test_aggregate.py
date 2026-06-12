# SPDX-License-Identifier: BSD-3-Clause
"""Tests for inter-cluster line aggregation."""

from __future__ import annotations

import pytest

from gtopt_reduce_network._aggregate import aggregate_lines


def test_two_parallel_inter_cluster_lines_combine() -> None:
    """Two lines (X=0.4, X=0.4) between clusters A,B → one X=0.2, F=110."""
    surviving_uids = [10, 11]
    a = [1, 1]
    b = [2, 2]
    x = [0.4, 0.4]
    r = [0.0, 0.0]
    fab = [50.0, 60.0]
    fba = [50.0, 60.0]
    cluster_of_bus = {1: 1, 2: 2}
    agg, linemap = aggregate_lines(surviving_uids, a, b, x, r, fab, fba, cluster_of_bus)
    assert len(agg) == 1
    assert agg[0].reactance == pytest.approx(0.2)
    assert agg[0].tmax_ab == pytest.approx(110.0)
    assert agg[0].tmax_ba == pytest.approx(110.0)
    rules = sorted({row.rule for row in linemap})
    assert rules == ["inter-cluster"]


def test_intra_cluster_lines_dropped() -> None:
    surviving_uids = [10, 11]
    a = [1, 2]
    b = [2, 3]
    x = [0.1, 0.1]
    r = [0.0, 0.0]
    fab = [50.0, 50.0]
    fba = [50.0, 50.0]
    # All three buses in one cluster.
    cluster_of_bus = {1: 1, 2: 1, 3: 1}
    agg, linemap = aggregate_lines(surviving_uids, a, b, x, r, fab, fba, cluster_of_bus)
    assert not agg
    assert {row.rule for row in linemap} == {"intra-cluster"}
    assert {row.original_line_uid for row in linemap} == {10, 11}


def test_directional_capacity_respects_endpoint_orientation() -> None:
    """Reverse one of two parallel lines; AB+BA must still be the row sum."""
    surviving_uids = [10, 11]
    a = [1, 2]  # second line is reversed (a=2, b=1)
    b = [2, 1]
    x = [0.1, 0.1]
    r = [0.0, 0.0]
    fab = [40.0, 60.0]
    fba = [70.0, 30.0]
    cluster_of_bus = {1: 1, 2: 2}
    agg, _ = aggregate_lines(surviving_uids, a, b, x, r, fab, fba, cluster_of_bus)
    # Cluster ordering (a,b) is sorted: (1,2). For uid=10 forward
    # contribution is fab[0]=40, fba[0]=70; uid=11 is reversed so its fab
    # in the cluster orientation is fba[1]=30 and fba is fab[1]=60.
    assert agg[0].tmax_ab == pytest.approx(40.0 + 30.0)
    assert agg[0].tmax_ba == pytest.approx(70.0 + 60.0)


def test_unknown_reactance_rule_raises() -> None:
    with pytest.raises(ValueError, match="unknown reactance rule"):
        aggregate_lines([], [], [], [], [], [], [], {}, rule="bogus")
