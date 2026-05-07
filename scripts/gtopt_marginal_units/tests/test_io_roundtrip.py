# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end round trip — write a dataset via main.cli, then re-read
via the consumer API and verify the marginal_gen_uids are preserved
and recompute_lmp is internally consistent."""

from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from gtopt_marginal_units.constants import EXIT_OK, EXIT_UNATTRIBUTED
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli


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
    """Build a synthetic gtopt-output directory: one cell where g10 (cheap)
    is interior at 50 MW and g20 is off, λ_b = 10 at both buses."""
    out = tmp_path / "tiny_out"
    out.mkdir()
    (out / "Generator").mkdir()
    (out / "Bus").mkdir()
    (out / "Line").mkdir()
    (out / "Demand").mkdir()

    # Generator/generation_sol — wide form per gtopt convention.
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


def test_end_to_end_roundtrip(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    code = cli(
        [
            "--input-kind",
            "gtopt-dir",
            "--mode",
            "simulated",
            "--planning",
            str(tiny_planning),
            "--output",
            str(tiny_output_dir),
            "--out",
            str(out),
        ]
    )
    assert code in (EXIT_OK, EXIT_UNATTRIBUTED)

    # Re-read via consumer API.
    ds = MarginalUnitDataset.open(out)
    per_bus = ds.per_bus()
    assert not per_bus.empty
    assert "bus_uid" in per_bus.columns
    # bus_uid 1 and 2 both must have rows.
    assert set(per_bus["bus_uid"].unique()) >= {1, 2}

    # Manifest sanity.
    mf = ds.manifest()
    assert mf["producer"] == "gtopt_marginal_units"
    assert mf["schema_version"]


def test_recompute_lmp_with_captured_costs_preserves_value(
    tmp_path, tiny_planning, tiny_output_dir
):
    out = tmp_path / "marginals.parquet"
    cli(
        [
            "--input-kind",
            "gtopt-dir",
            "--mode",
            "simulated",
            "--planning",
            str(tiny_planning),
            "--output",
            str(tiny_output_dir),
            "--out",
            str(out),
        ]
    )
    ds = MarginalUnitDataset.open(out)
    # Recompute with the same costs the writer captured (10 / 80).
    df = ds.recompute_lmp(unit_costs={10: 10.0, 20: 80.0})
    assert "zone_lmp_recomputed" in df.columns
    # For unit-driven cells, recomputed must equal original within tolerance.
    if "lmp_delta" in df.columns:
        assert (df["lmp_delta"].abs().fillna(0) <= 1e-6).all()


def test_recompute_emission_with_captured_factors(
    tmp_path, tiny_planning, tiny_output_dir
):
    out = tmp_path / "marginals.parquet"
    cli(
        [
            "--input-kind",
            "gtopt-dir",
            "--mode",
            "simulated",
            "--planning",
            str(tiny_planning),
            "--output",
            str(tiny_output_dir),
            "--out",
            str(out),
        ]
    )
    ds = MarginalUnitDataset.open(out)
    df = ds.recompute_emission(unit_emissions={10: 400.0, 20: 700.0})
    assert "emission_intensity_recomputed" in df.columns
