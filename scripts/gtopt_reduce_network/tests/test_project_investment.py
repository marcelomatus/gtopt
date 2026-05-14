# SPDX-License-Identifier: BSD-3-Clause
"""Tests for investment projection (per-cluster Δ → per-bus Δ)."""

from __future__ import annotations

from pathlib import Path

import pytest

from gtopt_reduce_network._busmap import (
    BusmapRow,
    LinemapRow,
    save_aggregator,
    save_busmap,
    save_linemap,
)
from gtopt_reduce_network._io import Case
from gtopt_reduce_network._project_investment import (
    ProjectInvestmentConfig,
    _largest_remainder_split,
    project_investment,
)


def _toy_case() -> Case:
    raw = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [
                {"uid": 1, "name": "b1"},
                {"uid": 2, "name": "b2"},
                {"uid": 3, "name": "b3"},
            ],
            "generator_array": [
                {"uid": 1, "bus": "b1", "pmax": 200, "capacity": 200},
                {"uid": 2, "bus": "b2", "pmax": 100, "capacity": 100},
                {"uid": 3, "bus": "b3", "pmax": 0, "capacity": 0},
            ],
            "demand_array": [
                {"uid": 1, "bus": "b1", "lmax": 50},
                {"uid": 2, "bus": "b2", "lmax": 30},
                {"uid": 3, "bus": "b3", "lmax": 10},
            ],
            "line_array": [],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    return case


def test_largest_remainder_preserves_sum_int() -> None:
    out = _largest_remainder_split(10, [1.0, 2.0, 3.0], integer=True)
    assert sum(out) == 10
    out2 = _largest_remainder_split(7, [1.0, 1.0, 1.0], integer=True)
    assert sum(out2) == 7
    # Largest weight gets the remainder.
    assert out[2] >= out[1] >= out[0]


def test_largest_remainder_zero_weights_split_evenly() -> None:
    out = _largest_remainder_split(9, [0.0, 0.0, 0.0], integer=True)
    assert sum(out) == 9


def test_existing_share_splits_by_capacity(tmp_path: Path) -> None:
    case = _toy_case()
    bm = [BusmapRow(1, 1), BusmapRow(2, 1), BusmapRow(3, 1)]
    save_busmap(bm, tmp_path / "busmap.csv")
    save_aggregator([], tmp_path / "agg.csv")
    save_linemap([], tmp_path / "lm.csv")
    reduced = {
        "generator_array": [
            {"uid": 99, "bus": 1, "expmod_delta": 30.0, "integer_expmod": False}
        ]
    }
    cfg = ProjectInvestmentConfig(share_mode="existing")
    patch, audit = project_investment(
        reduced,
        original_case=case,
        busmap=bm,
        aggregator=[],
        linemap=[],
        config=cfg,
    )
    by_bus = {
        row["bus"]: row["expmod_delta"] for row in patch["system"]["generator_array"]
    }
    # Capacities are 200, 100, 0 → shares 200/300, 100/300, 0/300 → 20, 10, 0.
    assert by_bus[1] == pytest.approx(20.0)
    assert by_bus[2] == pytest.approx(10.0)
    assert 3 not in by_bus
    assert sum(by_bus.values()) == pytest.approx(30.0)
    assert all(row["delta"] >= 0 for row in audit)


def test_load_share_uses_peak_load(tmp_path: Path) -> None:
    case = _toy_case()
    bm = [BusmapRow(1, 1), BusmapRow(2, 1), BusmapRow(3, 1)]
    reduced = {
        "generator_array": [
            {"uid": 99, "bus": 1, "expmod_delta": 90.0, "integer_expmod": False}
        ]
    }
    cfg = ProjectInvestmentConfig(share_mode="load")
    patch, _ = project_investment(
        reduced,
        original_case=case,
        busmap=bm,
        aggregator=[],
        linemap=[],
        config=cfg,
    )
    by_bus = {
        row["bus"]: row["expmod_delta"] for row in patch["system"]["generator_array"]
    }
    # Loads 50/30/10 → 90 split → 50, 30, 10.
    assert by_bus[1] == pytest.approx(50.0)
    assert by_bus[2] == pytest.approx(30.0)
    assert by_bus[3] == pytest.approx(10.0)


def test_line_split_by_capacity() -> None:
    case = _toy_case()
    case.array("line_array").extend(
        [
            {
                "uid": 10,
                "bus_a": 1,
                "bus_b": 2,
                "reactance": 0.1,
                "tmax_ab": 60,
                "tmax_ba": 60,
            },
            {
                "uid": 11,
                "bus_a": 1,
                "bus_b": 2,
                "reactance": 0.1,
                "tmax_ab": 40,
                "tmax_ba": 40,
            },
        ]
    )
    bm = [BusmapRow(1, 1), BusmapRow(2, 2), BusmapRow(3, 3)]
    lm = [
        LinemapRow(10, 100, "inter-cluster"),
        LinemapRow(11, 100, "inter-cluster"),
    ]
    reduced = {
        "line_array": [{"uid": 100, "expmod_delta": 100.0, "integer_expmod": False}]
    }
    cfg = ProjectInvestmentConfig(share_mode="existing", line_share_mode="capacity")
    patch, _ = project_investment(
        reduced,
        original_case=case,
        busmap=bm,
        aggregator=[],
        linemap=lm,
        config=cfg,
    )
    by_uid = {row["uid"]: row["expmod_delta"] for row in patch["system"]["line_array"]}
    # 60 / (60+40) = 0.6 → 60; 40/100 = 0.4 → 40.
    assert by_uid[10] == pytest.approx(60.0)
    assert by_uid[11] == pytest.approx(40.0)
