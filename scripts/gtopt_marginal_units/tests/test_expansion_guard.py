# SPDX-License-Identifier: BSD-3-Clause
"""ExpansionNotSupportedError test — master §3.2 v1 limitation."""

from __future__ import annotations


import pytest

from gtopt_marginal_units._gtopt_reader import topology_from_planning
from gtopt_marginal_units.errors import ExpansionNotSupportedError


def test_generator_with_expansion_raises():
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "generator_array": [
                {
                    "uid": 10,
                    "name": "g10",
                    "bus": "b1",
                    "pmin": 0,
                    "pmax": 100,
                    "gcost": 20,
                    "expansion": True,
                },
            ],
        }
    }
    with pytest.raises(ExpansionNotSupportedError, match="expansion"):
        topology_from_planning(planning)


def test_line_with_expansion_raises():
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}],
            "generator_array": [],
            "line_array": [
                {
                    "uid": 100,
                    "name": "l1",
                    "bus_a": "b1",
                    "bus_b": "b2",
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                    "expansion": True,
                },
            ],
        }
    }
    with pytest.raises(ExpansionNotSupportedError, match="expansion"):
        topology_from_planning(planning)


def test_topology_without_expansion_loads_clean():
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}],
            "generator_array": [
                {
                    "uid": 10,
                    "name": "g10",
                    "bus": "b1",
                    "pmin": 0,
                    "pmax": 100,
                    "gcost": 20,
                },
            ],
            "line_array": [
                {
                    "uid": 100,
                    "name": "l1",
                    "bus_a": "b1",
                    "bus_b": "b2",
                    "tmax_ab": 50,
                    "tmax_ba": 50,
                    "reactance": 0.05,
                },
            ],
        }
    }
    topo = topology_from_planning(planning)
    assert len(topo.buses) == 2
    assert len(topo.generators) == 1
    assert topo.generators[0].declared_MC == 20.0
