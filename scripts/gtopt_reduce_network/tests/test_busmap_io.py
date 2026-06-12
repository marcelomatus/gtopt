# SPDX-License-Identifier: BSD-3-Clause
"""Tests for busmap / linemap / aggregator CSV roundtrips and the
reducer config JSON writer."""

from __future__ import annotations

from pathlib import Path

from gtopt_reduce_network._busmap import (
    AggregatorRow,
    BusmapRow,
    LinemapRow,
    load_aggregator,
    load_busmap,
    load_linemap,
    load_reducer_config,
    save_aggregator,
    save_busmap,
    save_linemap,
    save_reducer_config,
)


def test_busmap_roundtrip(tmp_path: Path) -> None:
    rows = [BusmapRow(1, 1), BusmapRow(2, 1), BusmapRow(3, 3)]
    p = tmp_path / "busmap.csv"
    save_busmap(rows, p)
    again = load_busmap(p)
    assert again == rows


def test_linemap_roundtrip_with_intra(tmp_path: Path) -> None:
    rows = [
        LinemapRow(10, 100, "inter-cluster"),
        LinemapRow(11, None, "intra-cluster"),
        LinemapRow(12, 100, "parallel-merge"),
    ]
    p = tmp_path / "linemap.csv"
    save_linemap(rows, p)
    again = load_linemap(p)
    assert again == rows


def test_aggregator_roundtrip(tmp_path: Path) -> None:
    rows = [
        AggregatorRow("generator", 1, 5, 5, 1.0),
        AggregatorRow("demand", 2, 5, 5, 1.0),
        AggregatorRow("turbine", 3, 7, 5, 1.0),
    ]
    p = tmp_path / "agg.csv"
    save_aggregator(rows, p)
    again = load_aggregator(p)
    assert again == rows


def test_reducer_config_roundtrip(tmp_path: Path) -> None:
    cfg = {"target_buses": 50, "distance": "ptdf", "anchors": [1, 2, 3]}
    p = tmp_path / "cfg.json"
    save_reducer_config(cfg, p)
    again = load_reducer_config(p)
    assert again == cfg
