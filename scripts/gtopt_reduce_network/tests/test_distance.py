# SPDX-License-Identifier: BSD-3-Clause
"""Tests for distance metrics."""

from __future__ import annotations

import numpy as np
import pytest

from gtopt_reduce_network._distance import (
    ptdf_distance_matrix,
    reactance_shortest_path_matrix,
    zbus_distance_matrix,
)
from gtopt_reduce_network._io import Case
from gtopt_reduce_network._topology import build_line_graph


def _three_bus_chain() -> Case:
    raw = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [{"uid": i} for i in (1, 2, 3)],
            "line_array": [
                {
                    "uid": 10,
                    "bus_a": 1,
                    "bus_b": 2,
                    "reactance": 0.5,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
                {
                    "uid": 11,
                    "bus_a": 2,
                    "bus_b": 3,
                    "reactance": 0.7,
                    "tmax_ab": 100,
                    "tmax_ba": 100,
                },
            ],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    return case


def test_reactance_shortest_path_chain() -> None:
    case = _three_bus_chain()
    graph = build_line_graph(case)
    d = reactance_shortest_path_matrix(graph)
    assert d.shape == (3, 3)
    assert d[0, 1] == pytest.approx(0.5)
    assert d[1, 2] == pytest.approx(0.7)
    assert d[0, 2] == pytest.approx(1.2)
    assert (np.diag(d) == 0).all()


def test_zbus_distance_chain() -> None:
    case = _three_bus_chain()
    graph = build_line_graph(case)
    d = zbus_distance_matrix(graph)
    assert d.shape == (3, 3)
    assert d[0, 0] == 0.0
    # Z-bus distance for a series chain equals the reactance sum.
    assert d[0, 2] == pytest.approx(1.2, rel=1e-9)
    assert d[0, 1] == pytest.approx(0.5, rel=1e-9)


def test_ptdf_distance_returns_zero_self() -> None:
    case = _three_bus_chain()
    graph = build_line_graph(case)
    d = ptdf_distance_matrix(graph)
    assert d.shape == (3, 3)
    assert (np.diag(d) == 0).all()
    # Symmetric.
    assert np.allclose(d, d.T, atol=1e-9)


def test_distance_disconnected_components() -> None:
    """Two disjoint pairs → inf between components for shortest-path & zbus."""
    raw = {
        "options": {},
        "simulation": {},
        "system": {
            "bus_array": [{"uid": i} for i in (1, 2, 3, 4)],
            "line_array": [
                {
                    "uid": 10,
                    "bus_a": 1,
                    "bus_b": 2,
                    "reactance": 0.1,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
                {
                    "uid": 11,
                    "bus_a": 3,
                    "bus_b": 4,
                    "reactance": 0.1,
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                },
            ],
        },
    }
    case = Case(raw=raw)
    from gtopt_reduce_network._io import _index_buses

    _index_buses(case)
    graph = build_line_graph(case)
    d = reactance_shortest_path_matrix(graph)
    assert np.isinf(d[0, 2])
    z = zbus_distance_matrix(graph)
    assert np.isinf(z[0, 2])
