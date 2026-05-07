# SPDX-License-Identifier: BSD-3-Clause
"""Topology loader tests — CEN-format and gtopt-planning fallback."""

from __future__ import annotations

import json
from pathlib import Path


from cen2gtopt._topology import load_topology
from cen2gtopt._normalize import stable_uid


def test_load_cen_format(tmp_path: Path):
    cen = {
        "buses": [{"name": "Crucero"}, {"name": "Charrúa"}],
        "generators": [
            {
                "name": "BOCAMINA II",
                "bus": "Crucero",
                "pmin": 0,
                "pmax": 350,
                "declared_MC": 35.0,
                "kind": "thermal",
                "emission_factor": 950.0,
            },
        ],
        "lines": [
            {
                "name": "Crucero-Charrúa",
                "bus_a": "Crucero",
                "bus_b": "Charrúa",
                "tmax_ab": 1500,
                "tmax_ba": 1500,
                "reactance": 0.02,
            },
        ],
    }
    path = tmp_path / "topo.json"
    path.write_text(json.dumps(cen), encoding="utf-8")
    topo = load_topology(path)
    assert len(topo.buses) == 2
    assert len(topo.generators) == 1
    assert topo.generators[0].declared_MC == 35.0
    # UID stability: re-loading produces the same UID.
    assert topo.buses[0].uid == stable_uid("Crucero")


def test_load_gtopt_planning_fallback(tmp_path: Path):
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
                    "gcost": 25.0,
                },
            ],
            "line_array": [],
        }
    }
    path = tmp_path / "planning.json"
    path.write_text(json.dumps(planning), encoding="utf-8")
    topo = load_topology(path)
    assert len(topo.buses) == 2
    assert topo.generators[0].declared_MC == 25.0
