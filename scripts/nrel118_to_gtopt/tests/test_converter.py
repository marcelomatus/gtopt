"""Unit tests for the NREL-118 → gtopt converter."""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from nrel118_to_gtopt._converter import (
    BTU_PER_KWH_TO_GJ_PER_MWH,
    IPCC_AR6_TCO2_PER_GJ,
    collapse_piecewise_heat_rate,
    to_gtopt_json,
    unit_type_to_fuel,
)
from nrel118_to_gtopt.__main__ import populate_cache


def test_btu_per_kwh_conversion_constant():
    """Sanity-check the BTU/kWh → GJ/MWh constant: 0.001055."""

    assert math.isclose(BTU_PER_KWH_TO_GJ_PER_MWH, 1.055e-3, rel_tol=1e-6)


def test_ipcc_factors_coal_natural_gas_diesel():
    """Cross-check against IPCC AR6 Table A.III.2 (rounded to 4 dp)."""

    assert IPCC_AR6_TCO2_PER_GJ["coal"] == pytest.approx(0.0946, abs=1e-6)
    assert IPCC_AR6_TCO2_PER_GJ["natural_gas"] == pytest.approx(0.0561, abs=1e-6)
    assert IPCC_AR6_TCO2_PER_GJ["diesel"] == pytest.approx(0.0741, abs=1e-6)
    # Renewables / nuclear must be exactly zero.
    for renewable in ("wind", "solar", "hydro", "nuclear", "biomass"):
        assert IPCC_AR6_TCO2_PER_GJ[renewable] == 0.0


def test_unit_type_to_fuel_mapping_covers_nrel118_categories():
    """Every category that appears in NREL-118 must map to a known fuel."""

    cases = {
        "Biomass 01": "biomass",
        "CC NG 02": "natural_gas",
        "CT NG 03": "natural_gas",
        "CT Oil 04": "diesel",
        "Geo 01": "geothermal",
        "Hydro 12": "hydro",
        "ICE NG 05": "natural_gas",
        "Solar 18": "solar",
        "ST Coal 06": "coal",
        "ST NG 07": "natural_gas",
        "ST Other 08": "other",
        "Wind 09": "wind",
    }
    for name, expected_kind in cases.items():
        assert unit_type_to_fuel(name) == expected_kind, name


def test_collapse_piecewise_heat_rate_single_band():
    """Single-band collapse: ST_Coal_01 in NREL-118.

    Generators.csv:  HR_Base=108.8 MMBTU/hr, Inc1=10171.43 BTU/kWh,
                     pmin=19.84 MW (= Load Point Band 1 start), pmax=20.
    """

    hr_gj_per_mwh = collapse_piecewise_heat_rate(
        hr_base_mmbtu_per_hr=108.8,
        hr_inc_bands_btu_per_kwh=[10171.43, 0, 0, 0, 0],
        load_points_mw=[19.84, 0, 0, 0, 0],
        pmin=19.84,
        pmax=20.0,
    )
    # No incremental band fires (LP1 == pmin), so HR = HR_Base / pmax × 1.055.
    expected = 108.8 / 20.0 * 1.055
    assert hr_gj_per_mwh == pytest.approx(expected, rel=1e-5)


def test_collapse_piecewise_heat_rate_with_band_extension():
    """Real CC_NG_01: pmin = LP1 = pmax, so HR_Base alone determines HR."""

    hr = collapse_piecewise_heat_rate(
        hr_base_mmbtu_per_hr=316.78,
        hr_inc_bands_btu_per_kwh=[6489.0, 0, 0, 0, 0],
        load_points_mw=[41.5, 0, 0, 0, 0],
        pmin=41.08,
        pmax=41.5,
    )
    # HR_Base contribution alone = 316.78 / 41.5 = 7.633 MMBTU/MWh →
    # 7.633 × 1.055 = 8.053 GJ/MWh.  Plus a small Inc1 contribution
    # over the (41.5 − 41.08) MW band.
    base = 316.78 / 41.5 * 1.055
    assert hr == pytest.approx(base, rel=0.01)


def test_collapse_piecewise_heat_rate_zero_pmax_returns_zero():
    """Defensive guard: pmax <= 0 → 0 (no division-by-zero)."""

    assert (
        collapse_piecewise_heat_rate(0, [0, 0, 0, 0, 0], [0, 0, 0, 0, 0], 0, 0) == 0.0
    )


def test_to_gtopt_json_baseline_has_no_renewable_gen(tmp_path: Path):
    """``renewables_share=0`` → no synthetic aggregate_renewables generator."""

    from nrel118_to_gtopt._converter import Conversion, NrelGen

    conv = Conversion(
        week=1,
        n_hours=2,
        bus_count=1,
        gen_count=1,
        line_count=0,
        total_load_mw=[100.0, 90.0],
        fuels=[],
        generators=[
            NrelGen(
                name="Solar 01",
                bus="bus1",
                pmax=200.0,
                pmin=0.0,
                vom=0.0,
                fuel="solar",
                heat_rate_gj_per_mwh=0.0,
                is_renewable=True,
            ),
        ],
    )
    payload = to_gtopt_json(conv, renewables_share=0.0)
    names = {g["name"] for g in payload["system"]["generator_array"]}
    assert "aggregate_renewables" not in names
    assert "Solar 01" in names


def test_to_gtopt_json_with_renewables_share_adds_aggregate(tmp_path: Path):
    """``renewables_share=0.33`` → adds the aggregate renewable + derates."""

    from nrel118_to_gtopt._converter import Conversion, NrelGen

    conv = Conversion(
        week=1,
        n_hours=2,
        bus_count=1,
        gen_count=1,
        line_count=0,
        total_load_mw=[100.0, 100.0],
        fuels=[],
        generators=[
            NrelGen(
                name="ST Coal 01",
                bus="bus1",
                pmax=100.0,
                pmin=0.0,
                vom=2.99,
                fuel="coal",
                heat_rate_gj_per_mwh=10.7,
                is_renewable=False,
            ),
        ],
    )
    payload = to_gtopt_json(conv, renewables_share=0.33)
    coal = next(
        g for g in payload["system"]["generator_array"] if g["name"] == "ST Coal 01"
    )
    # Capacity derated by (1 − 0.33).
    assert coal["capacity"] == pytest.approx(100.0 * 0.67, rel=1e-9)
    renew = next(
        g
        for g in payload["system"]["generator_array"]
        if g["name"] == "aggregate_renewables"
    )
    # Aggregate renewable capacity = peak × share.
    assert renew["capacity"] == pytest.approx(100.0 * 0.33, rel=1e-9)


# ────────────────────────────────────────────────────────────────────
# Regression — Fix 1 (2026-06): without a finite VOLL, the LP curtailed
# every MW of demand for free and dispatched nothing (obj = 0, empty
# generation_sol).  The converter MUST emit an ``options.model_options``
# block with at minimum ``demand_fail_cost`` and ``use_single_bus``.
# ────────────────────────────────────────────────────────────────────


def test_to_gtopt_json_sets_options_model_options():
    """``options.model_options`` must be present with VOLL + single-bus."""

    from nrel118_to_gtopt._converter import Conversion, NrelGen

    conv = Conversion(
        week=1,
        n_hours=2,
        bus_count=1,
        gen_count=1,
        line_count=0,
        total_load_mw=[100.0, 100.0],
        fuels=[],
        generators=[
            NrelGen(
                name="ST Coal 01",
                bus="bus1",
                pmax=100.0,
                pmin=0.0,
                vom=2.99,
                fuel="coal",
                heat_rate_gj_per_mwh=10.7,
                is_renewable=False,
            ),
        ],
    )
    payload = to_gtopt_json(conv)
    assert "options" in payload, "missing top-level 'options' key"
    mo = payload["options"].get("model_options")
    assert mo is not None, "missing options.model_options"
    # VOLL must be finite and large enough to drive dispatch over
    # the most expensive thermal in the NREL-118 fleet (≈ $140/MWh).
    assert mo.get("demand_fail_cost", 0.0) >= 500.0, (
        "demand_fail_cost too low — LP will silently curtail demand and "
        "dispatch nothing (the regressed behaviour we are guarding against)"
    )
    # Single-bus mode is asserted because the converter aggregates to
    # one bus by construction.
    assert mo.get("use_single_bus") is True


def _gtopt_binary() -> str | None:
    """Locate the gtopt executable for end-to-end smoke tests."""

    env = os.environ.get("GTOPT_BIN")
    if env and Path(env).is_file():
        return env
    for candidate in (
        Path.cwd() / "build" / "standalone" / "gtopt",
        Path(__file__).resolve().parents[3] / "build" / "standalone" / "gtopt",
    ):
        if candidate.is_file():
            return str(candidate)
    return shutil.which("gtopt")


def _sum_value_column(parquet_dir: Path) -> float:
    """Sum the ``value`` column across every Parquet partition under dir."""

    try:
        import pyarrow.parquet as pq
    except ImportError:  # pragma: no cover
        return 0.0

    total = 0.0
    if not parquet_dir.is_dir():
        return 0.0
    for pf in parquet_dir.rglob("*.parquet"):
        t = pq.read_table(pf)
        if "value" not in t.column_names:
            continue
        total += sum(v for v in t.column("value").to_pylist() if v is not None)
    return total


@pytest.mark.integration
def test_nrel118_week_dispatches_under_voll(tmp_path: Path):
    """End-to-end Fix 1 regression: gtopt --lp-only must build a non-trivial
    LP on the NREL-118 1-week JSON.

    Skipped when the gtopt binary or NREL-118 cache aren't available
    (CI environments without the build artefacts or network access).
    """

    binary = _gtopt_binary()
    if binary is None:
        pytest.skip("gtopt binary not found — set GTOPT_BIN or build first")

    cache_dir = Path.home() / ".cache" / "gtopt" / "nrel118"
    try:
        populate_cache(cache_dir)
    except OSError as exc:  # pragma: no cover - network failure path
        pytest.skip(f"NREL-118 cache not available: {exc}")

    from nrel118_to_gtopt._converter import convert

    conversion = convert(cache_dir, week=2)
    payload = to_gtopt_json(conversion, renewables_share=0.0)
    out = tmp_path / "baseline.json"
    out.write_text(json.dumps(payload), encoding="utf-8")

    # Smoke-build the LP without solving — confirms gtopt accepts our
    # options block + that the LP carries a non-empty objective.
    result = subprocess.run(
        [binary, "-s", str(out), "--lp-only"],
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    assert result.returncode == 0, (
        "gtopt --lp-only failed: stderr=\n" + result.stderr[-2000:]
    )


@pytest.mark.integration
def test_nrel118_week_co2_reduction_in_expected_window(tmp_path: Path):
    """Full Fix 1 e2e: solve baseline + renewables-share=0.33 and assert the
    CO2-reduction window aligns with Peña, Martinez-Anido, Hodge 2017
    (annual reference 29-34%, with a wider [15%, 50%] tolerance for the
    1-week-winter slice).

    Skipped when the binary, cache or Parquet reader aren't available.
    """

    binary = _gtopt_binary()
    if binary is None:
        pytest.skip("gtopt binary not found — set GTOPT_BIN or build first")
    pyarrow = pytest.importorskip("pyarrow.parquet")
    del pyarrow  # silence unused-import linters; imported lazily by helper

    cache_dir = Path.home() / ".cache" / "gtopt" / "nrel118"
    try:
        populate_cache(cache_dir)
    except OSError as exc:  # pragma: no cover - network failure path
        pytest.skip(f"NREL-118 cache not available: {exc}")

    from nrel118_to_gtopt._converter import convert

    conversion = convert(cache_dir, week=2)

    totals: dict[str, tuple[float, float]] = {}
    for name, share in (("baseline", 0.0), ("ren33", 0.33)):
        payload = to_gtopt_json(conversion, renewables_share=share)
        jpath = tmp_path / f"{name}.json"
        sol_dir = tmp_path / f"sol_{name}"
        jpath.write_text(json.dumps(payload), encoding="utf-8")
        result = subprocess.run(
            [binary, "-s", str(jpath), "--no-mip", "-d", str(sol_dir)],
            capture_output=True,
            text=True,
            check=False,
            timeout=300,
        )
        assert result.returncode == 0, (
            f"gtopt solve failed for {name}: stderr=\n" + result.stderr[-2000:]
        )
        gen = _sum_value_column(sol_dir / "Generator" / "generation_sol.parquet")
        co2 = _sum_value_column(sol_dir / "EmissionSource" / "emissions_sol.parquet")
        totals[name] = (gen, co2)

    b_gen, b_co2 = totals["baseline"]
    _, r_co2 = totals["ren33"]
    assert b_gen > 0.0, "baseline produced no generation"
    assert b_co2 > 0.0, (
        "baseline produced no CO2 — converter must emit thermal "
        "generators with non-trivial fuel SRMC + hydro CF"
    )
    pct_reduction = 100.0 * (b_co2 - r_co2) / b_co2
    assert 15.0 <= pct_reduction <= 50.0, (
        f"% reduction {pct_reduction:.1f}% outside [15%, 50%] window: "
        f"baseline={b_co2:.0f} tCO2 ren33={r_co2:.0f} tCO2"
    )
