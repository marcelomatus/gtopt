# SPDX-License-Identifier: BSD-3-Clause
"""Shared pytest fixtures for ``gtopt_marginal_units`` tests.

These fixtures build a 2-bus / 2-generator / 1-line synthetic case and a
matching gtopt-output directory under ``tmp_path``.  They are used by
``test_io_roundtrip`` and ``test_carbon_price``; lifting them here lets
multiple test modules share the setup without cross-test-module imports
(which trigger ruff F811 false positives).
"""

from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest


@pytest.fixture
def tiny_planning(tmp_path: Path) -> Path:
    """Minimal planning JSON: 2 buses, 2 generators, 1 line."""
    planning = {
        "options": {"demand_fail_cost": 1000.0, "scale_objective": 1.0},
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1.0}],
            "stage_array": [{"uid": 1}],
            "scenario_array": [{"uid": 1}],
        },
        "system": {
            "name": "tiny",
            "bus_array": [
                {"uid": 1, "name": "b1"},
                {"uid": 2, "name": "b2"},
            ],
            "generator_array": [
                {
                    "uid": 10,
                    "name": "cheap",
                    "bus": "b1",
                    "pmin": 0,
                    "pmax": 100,
                    "gcost": 10.0,
                    "emission_factor": 400.0,
                    "type": "thermal",
                },
                {
                    "uid": 20,
                    "name": "peaker",
                    "bus": "b2",
                    "pmin": 0,
                    "pmax": 100,
                    "gcost": 80.0,
                    "emission_factor": 700.0,
                    "type": "thermal",
                },
            ],
            "line_array": [
                {
                    "uid": 100,
                    "name": "l1_2",
                    "bus_a": "b1",
                    "bus_b": "b2",
                    "tmax_ab": 200,
                    "tmax_ba": 200,
                    "reactance": 0.05,
                },
            ],
        },
    }
    path = tmp_path / "planning.json"
    path.write_text(json.dumps(planning))
    return path


@pytest.fixture
def tiny_output_dir(tmp_path: Path) -> Path:
    """Build a synthetic gtopt-output directory.

    One cell where g10 (cheap) is interior at 50 MW and g20 is off,
    λ_b = 10 at both buses, all 50 MW of demand at bus 2.
    """
    out = tmp_path / "tiny_out"
    out.mkdir()
    (out / "Generator").mkdir()
    (out / "Bus").mkdir()
    (out / "Line").mkdir()
    (out / "Demand").mkdir()

    pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:10": [50.0],
            "uid:20": [0.0],
        }
    ).to_csv(out / "Generator/generation_sol.csv", index=False)

    pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [10.0],
            "uid:2": [10.0],
        }
    ).to_csv(out / "Bus/balance_dual.csv", index=False)

    pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:100": [50.0],
        }
    ).to_csv(out / "Line/flowp_sol.csv", index=False)

    pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:100": [0.0],
        }
    ).to_csv(out / "Line/flowp_cost.csv", index=False)

    pd.DataFrame(
        {
            "scenario": [1],
            "stage": [1],
            "block": [1],
            "uid:1": [0.0],
            "uid:2": [50.0],
        }
    ).to_csv(out / "Demand/load_sol.csv", index=False)

    return out
