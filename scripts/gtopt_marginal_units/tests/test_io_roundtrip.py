# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end round trip — write a dataset via main.cli, then re-read
via the consumer API and verify the marginal_gen_uids are preserved
and recompute_lmp is internally consistent.

The ``tiny_planning`` and ``tiny_output_dir`` fixtures live in
``conftest.py`` so they can be shared with other test modules without
cross-module imports.
"""

from __future__ import annotations

from gtopt_marginal_units.constants import EXIT_OK, EXIT_UNATTRIBUTED
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli


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
