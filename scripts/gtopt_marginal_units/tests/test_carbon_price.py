# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the ``--carbon-price`` knob (master plan v1.1 deferral).

Covers:
* CLI flag parsing + persistence to ``manifest.extras``.
* Negative carbon prices rejected at the CLI.
* Consumer API reads the manifest value when no override is given.
* ``bus_lmp(carbon_price=p)`` adds ``lmp_with_carbon = zone_lmp + p/1000 * ε``.
* Override at the consumer call site beats the manifest value.
* ``recompute_lmp(..., carbon_price=p)`` composes correctly.
* ``bus_lmp_and_emission(carbon_price=p)`` exposes the same adder.
* When ``carbon_price`` is absent / 0, output is identical to the legacy path
  (no extra columns, no row count change).
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from gtopt_marginal_units.constants import EXIT_INPUT_ERROR
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli

# ``tiny_planning`` / ``tiny_output_dir`` come from conftest.py.


def _run_cli(
    *,
    planning: Path,
    output: Path,
    out: Path,
    carbon_price: float | None = None,
) -> int:
    argv = [
        "--input-kind",
        "gtopt-dir",
        "--mode",
        "simulated",
        "--planning",
        str(planning),
        "--output",
        str(output),
        "--out",
        str(out),
    ]
    if carbon_price is not None:
        argv += ["--carbon-price", str(carbon_price)]
    return cli(argv)


# ---------------------------------------------------------------------------
# CLI persistence
# ---------------------------------------------------------------------------


def test_default_no_carbon_price_in_manifest(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    mf = json.loads((out / "manifest.json").read_text())
    extras = mf.get("extras") or {}
    # Absent — manifest extras should not advertise a carbon price when the
    # user did not opt in.
    assert "carbon_price_usd_per_ton" not in extras


def test_positive_carbon_price_persisted(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(
        planning=tiny_planning,
        output=tiny_output_dir,
        out=out,
        carbon_price=50.0,
    )
    mf = json.loads((out / "manifest.json").read_text())
    extras = mf["extras"]
    assert extras["carbon_price_usd_per_ton"] == pytest.approx(50.0)


def test_zero_carbon_price_is_not_persisted(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(
        planning=tiny_planning,
        output=tiny_output_dir,
        out=out,
        carbon_price=0.0,
    )
    mf = json.loads((out / "manifest.json").read_text())
    extras = mf.get("extras") or {}
    # Zero is the silent default; we don't pollute the manifest.
    assert "carbon_price_usd_per_ton" not in extras


def test_negative_carbon_price_rejected(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    code = _run_cli(
        planning=tiny_planning,
        output=tiny_output_dir,
        out=out,
        carbon_price=-1.0,
    )
    assert code == EXIT_INPUT_ERROR


# ---------------------------------------------------------------------------
# Consumer API — manifest fallback
# ---------------------------------------------------------------------------


def test_carbon_price_manifest_default_is_zero(
    tmp_path, tiny_planning, tiny_output_dir
):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    assert ds.carbon_price_usd_per_ton() == 0.0


def test_carbon_price_manifest_round_trip(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(
        planning=tiny_planning,
        output=tiny_output_dir,
        out=out,
        carbon_price=80.0,
    )
    ds = MarginalUnitDataset.open(out)
    assert ds.carbon_price_usd_per_ton() == pytest.approx(80.0)


# ---------------------------------------------------------------------------
# bus_lmp with carbon price
# ---------------------------------------------------------------------------


def test_bus_lmp_no_carbon_columns_when_unset(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    df = ds.bus_lmp()
    # Legacy path — no carbon columns.
    assert "lmp_with_carbon" not in df.columns
    assert "carbon_adder_usd_per_mwh" not in df.columns
    assert "carbon_price_usd_per_ton" not in df.columns


def test_bus_lmp_explicit_zero_no_carbon_columns(
    tmp_path, tiny_planning, tiny_output_dir
):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    # Explicit 0.0 must behave like the silent default.
    df = ds.bus_lmp(carbon_price=0.0)
    assert "lmp_with_carbon" not in df.columns


def test_bus_lmp_with_carbon_emits_adder_column(
    tmp_path, tiny_planning, tiny_output_dir
):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    df = ds.bus_lmp(carbon_price=100.0)
    assert "lmp_with_carbon" in df.columns
    assert "carbon_adder_usd_per_mwh" in df.columns
    assert "carbon_price_usd_per_ton" in df.columns
    assert (df["carbon_price_usd_per_ton"] == 100.0).all()
    # carbon_adder ≥ 0 when ε ≥ 0.
    assert (df["carbon_adder_usd_per_mwh"].fillna(0) >= 0).all()


def test_bus_lmp_carbon_price_arithmetic(tmp_path, tiny_planning, tiny_output_dir):
    """ε of the marginal generator (g10, 400 kg/MWh) at p=100 USD/ton
    must add 0.1 * 400 = 40 USD/MWh."""
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    df = ds.bus_lmp(carbon_price=100.0)
    # Bus 1 (where g10 is interior) — λ=10, ε=400 → adder=40, total=50.
    bus1 = df[df["bus_uid"] == 1]
    assert not bus1.empty
    row = bus1.iloc[0]
    assert row["zone_lmp"] == pytest.approx(10.0)
    assert row["emission_intensity_kg_per_mwh"] == pytest.approx(400.0)
    assert row["carbon_adder_usd_per_mwh"] == pytest.approx(40.0)
    assert row["lmp_with_carbon"] == pytest.approx(50.0)


def test_bus_lmp_override_beats_manifest(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(
        planning=tiny_planning,
        output=tiny_output_dir,
        out=out,
        carbon_price=20.0,
    )
    ds = MarginalUnitDataset.open(out)
    # Manifest says 20, but call-site says 100.
    df = ds.bus_lmp(carbon_price=100.0)
    assert (df["carbon_price_usd_per_ton"] == 100.0).all()


def test_bus_lmp_negative_override_rejected(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    with pytest.raises(ValueError, match="non-negative"):
        ds.bus_lmp(carbon_price=-1.0)


# ---------------------------------------------------------------------------
# recompute_lmp with carbon price
# ---------------------------------------------------------------------------


def test_recompute_lmp_with_carbon_adds_column(
    tmp_path, tiny_planning, tiny_output_dir
):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    df = ds.recompute_lmp(unit_costs={10: 10.0, 20: 80.0}, carbon_price=100.0)
    assert "lmp_with_carbon" in df.columns
    assert "carbon_adder_usd_per_mwh" in df.columns
    # Captured costs match the writer's catalogue, so zone_lmp_recomputed
    # must equal zone_lmp; carbon adder is identical to bus_lmp's.
    assert "zone_lmp_recomputed" in df.columns


def test_recompute_lmp_no_carbon_no_extra_columns(
    tmp_path, tiny_planning, tiny_output_dir
):
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    df = ds.recompute_lmp(unit_costs={10: 10.0, 20: 80.0})
    assert "lmp_with_carbon" not in df.columns
    assert "carbon_adder_usd_per_mwh" not in df.columns


def test_recompute_lmp_with_carbon_uses_unit_emissions_override(
    tmp_path, tiny_planning, tiny_output_dir
):
    """Doubling the marginal unit's EF doubles the carbon adder."""
    out = tmp_path / "marginals.parquet"
    _run_cli(planning=tiny_planning, output=tiny_output_dir, out=out)
    ds = MarginalUnitDataset.open(out)
    base = ds.recompute_lmp(
        unit_costs={10: 10.0, 20: 80.0},
        carbon_price=100.0,
    )
    doubled = ds.recompute_lmp(
        unit_costs={10: 10.0, 20: 80.0},
        carbon_price=100.0,
        unit_emissions={10: 800.0, 20: 1400.0},
    )
    bus1_base = base[base["bus_uid"] == 1].iloc[0]
    bus1_doubled = doubled[doubled["bus_uid"] == 1].iloc[0]
    assert bus1_doubled["carbon_adder_usd_per_mwh"] == pytest.approx(
        2 * bus1_base["carbon_adder_usd_per_mwh"]
    )


# ---------------------------------------------------------------------------
# bus_lmp_and_emission with carbon price
# ---------------------------------------------------------------------------


def test_bus_lmp_and_emission_carbon_columns(tmp_path, tiny_planning, tiny_output_dir):
    out = tmp_path / "marginals.parquet"
    _run_cli(
        planning=tiny_planning,
        output=tiny_output_dir,
        out=out,
        carbon_price=50.0,
    )
    ds = MarginalUnitDataset.open(out)
    df = ds.bus_lmp_and_emission()
    # Manifest carbon price = 50 → adder columns present.
    assert "lmp_with_carbon" in df.columns
    assert "carbon_adder_usd_per_mwh" in df.columns
    assert (df["carbon_price_usd_per_ton"] == 50.0).all()
    # Adder consistency.
    expected = (50.0 / 1000.0) * df["emission_intensity_kg_per_mwh"].fillna(0.0)
    assert (df["carbon_adder_usd_per_mwh"] - expected).abs().max() < 1e-9
