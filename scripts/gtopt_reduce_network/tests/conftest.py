# SPDX-License-Identifier: BSD-3-Clause
"""Shared fixtures for gtopt_reduce_network tests."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from gtopt_reduce_network._io import Case


@pytest.fixture
def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


@pytest.fixture
def ieee14_path(repo_root: Path) -> Path:
    return repo_root / "cases" / "ieee_14b" / "ieee_14b.json"


@pytest.fixture
def ieee57_path(repo_root: Path) -> Path:
    return repo_root / "cases" / "ieee_57b" / "ieee_57b.json"


@pytest.fixture
def tiny_chain_case() -> Case:
    """A 4-bus chain with one no-injection middle bus and parallel ties.

        b1 ── b2 ══ b3 ── b4
                (parallel)

    Generators on b1, demand on b4.  b3 has no injection.
    """
    raw: dict[str, Any] = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [
                {"uid": 1, "name": "b1"},
                {"uid": 2, "name": "b2"},
                {"uid": 3, "name": "b3"},
                {"uid": 4, "name": "b4"},
            ],
            "line_array": [
                {
                    "uid": 10,
                    "name": "l_b1_b2",
                    "bus_a": "b1",
                    "bus_b": "b2",
                    "reactance": 0.1,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
                {
                    "uid": 11,
                    "name": "l_b2_b3_a",
                    "bus_a": "b2",
                    "bus_b": "b3",
                    "reactance": 0.2,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
                {
                    "uid": 12,
                    "name": "l_b2_b3_b",
                    "bus_a": "b2",
                    "bus_b": "b3",
                    "reactance": 0.2,
                    "tmax_ab": 60,
                    "tmax_ba": 60,
                },
                {
                    "uid": 13,
                    "name": "l_b3_b4",
                    "bus_a": "b3",
                    "bus_b": "b4",
                    "reactance": 0.3,
                    "tmax_ab": 40,
                    "tmax_ba": 40,
                },
            ],
            "generator_array": [
                {
                    "uid": 1,
                    "name": "g1",
                    "bus": "b1",
                    "pmin": 0,
                    "pmax": 200,
                    "gcost": 10,
                    "capacity": 200,
                },
            ],
            "demand_array": [
                {"uid": 1, "name": "d4", "bus": "b4", "lmax": 80},
            ],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    return case


@pytest.fixture
def two_clusters_case() -> Case:
    """Two natural clusters joined by one weak tie, anchored at b1 and b6.

    b1 ── b2 ── b3
          │
          tie (high X)
          │
    b4 ── b5 ── b6
    """
    raw: dict[str, Any] = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [{"uid": i, "name": f"b{i}"} for i in range(1, 7)],
            "line_array": [
                {
                    "uid": 1,
                    "bus_a": "b1",
                    "bus_b": "b2",
                    "reactance": 0.05,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
                {
                    "uid": 2,
                    "bus_a": "b2",
                    "bus_b": "b3",
                    "reactance": 0.05,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
                {
                    "uid": 3,
                    "bus_a": "b2",
                    "bus_b": "b5",
                    "reactance": 5.0,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
                {
                    "uid": 4,
                    "bus_a": "b4",
                    "bus_b": "b5",
                    "reactance": 0.05,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
                {
                    "uid": 5,
                    "bus_a": "b5",
                    "bus_b": "b6",
                    "reactance": 0.05,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
            ],
            "generator_array": [
                {"uid": 1, "bus": "b1", "pmax": 500, "capacity": 500},
                {"uid": 2, "bus": "b6", "pmax": 500, "capacity": 500},
            ],
            "demand_array": [
                {"uid": 1, "bus": "b3", "lmax": 100},
                {"uid": 2, "bus": "b4", "lmax": 100},
            ],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    return case
