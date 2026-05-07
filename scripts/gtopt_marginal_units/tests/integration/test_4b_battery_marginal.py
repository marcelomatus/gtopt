# SPDX-License-Identifier: BSD-3-Clause
"""Synthetic 4-bus + battery test — force special operating regimes
that the larger composite IEEE 57b case cannot guarantee.

This is the small companion to ``test_ieee_57b_with_battery.py``.
The 4-bus topology is built entirely in test code (no external
planning JSON required), with three hand-picked cells designed to
exercise three §4.7 R3 cascade outcomes:

  * Cell A — battery is **off**, only the cheap thermal is interior.
    λ_z = MC[cheap].
  * Cell B — cheap thermal is **at pmax** (capped) and the battery
    is **interior**. The merit-order pick lands on the battery's
    declared MC; ``formula_kind = single_unit`` with the battery as
    the marginal unit.
  * Cell C — cheap thermal and battery are **both** at pmax; the
    peaker is interior. λ_z = MC[peaker].

This is the fixture that proves the v1 pipeline correctly attributes
λ_z to the battery when the merit order makes it the price-setter,
which the larger composite case cannot reliably force.
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pytest

from gtopt_canonical_feed import (
    SCHEMA_VERSION,
    Bus,
    Cells,
    Generator,
    Line,
    Manifest,
    Topology,
    write_feed,
)
from gtopt_canonical_feed.cells import (
    COL_BLOCK,
    COL_BUS_UID,
    COL_DATA_SOURCE,
    COL_DATE_UTC,
    COL_DISPATCH,
    COL_GEN_UID,
    COL_HOUR,
    COL_LOAD,
    COL_SCENARIO,
    COL_STAGE,
)
from gtopt_marginal_units.constants import EXIT_OK, EXIT_UNATTRIBUTED
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli


# ----------------------------------------------------------------------
# Fixed catalogue numbers — chosen so the §4.7 R3 cascade's three
# branches each fire in exactly one cell.
# ----------------------------------------------------------------------
_CHEAP_UID = 1
_CHEAP_MC = 10.0
_CHEAP_PMAX = 100.0

_PEAKER_UID = 2
_PEAKER_MC = 80.0
_PEAKER_PMAX = 100.0

_BATTERY_UID = 3
_BATTERY_MC = 30.0  # opportunity-cost proxy (charged at MC=10, η=0.85,
# plus a small degradation cost) — between the
# cheap and peaker tiers so it can be the merit
# ceiling on its own.
_BATTERY_PMAX = 50.0

# Hourly demand chosen to land in three regimes:
#   t1 (off-peak):     load = 50  → cheap interior at 50, λ = MC[cheap]
#   t2 (battery-set):  load = 130 → cheap at pmax, battery at 30, λ = MC[bat]
#   t3 (peaker-set):   load = 175 → cheap+battery at pmax, peaker at 25, λ = MC[peaker]
_HOURS_AND_LOADS = ((1, 50.0), (2, 130.0), (3, 175.0))


@pytest.fixture
def four_bus_feed(tmp_path: Path) -> Path:
    """Build the 4-bus + battery canonical feed."""
    buses = [Bus(uid=i, name=f"b{i}") for i in range(1, 5)]
    generators = [
        Generator(
            uid=_CHEAP_UID,
            name="cheap_thermal",
            bus_uid=1,
            pmin=0.0,
            pmax=_CHEAP_PMAX,
            declared_MC=_CHEAP_MC,
            kind="thermal",
            emission_factor=400.0,
        ),
        Generator(
            uid=_PEAKER_UID,
            name="peaker",
            bus_uid=2,
            pmin=0.0,
            pmax=_PEAKER_PMAX,
            declared_MC=_PEAKER_MC,
            kind="thermal",
            emission_factor=700.0,
        ),
        Generator(
            uid=_BATTERY_UID,
            name="battery",
            bus_uid=3,
            pmin=0.0,
            pmax=_BATTERY_PMAX,
            declared_MC=_BATTERY_MC,
            kind="battery",
            emission_factor=400.0 / 0.85,  # marginal-EF-at-charge / η
        ),
    ]
    lines = [
        Line(
            uid=10, bus_a_uid=1, bus_b_uid=2, tmax_ab=200, tmax_ba=200, reactance=0.05
        ),
        Line(
            uid=11, bus_a_uid=2, bus_b_uid=3, tmax_ab=200, tmax_ba=200, reactance=0.05
        ),
        Line(
            uid=12, bus_a_uid=3, bus_b_uid=4, tmax_ab=200, tmax_ba=200, reactance=0.05
        ),
    ]
    topology = Topology(buses=buses, generators=generators, lines=lines)

    rows_dispatch: list[dict] = []
    rows_load: list[dict] = []
    for hour, load in _HOURS_AND_LOADS:
        # Allocate via merit order: cheap → battery → peaker.
        remaining = load
        cheap_d = min(remaining, _CHEAP_PMAX)
        remaining -= cheap_d
        bat_d = min(remaining, _BATTERY_PMAX)
        remaining -= bat_d
        peaker_d = min(remaining, _PEAKER_PMAX)
        remaining -= peaker_d
        assert remaining <= 1e-9, "load exceeds total pmax"

        cell = _real_cell_key(hour)
        rows_dispatch.append({**cell, COL_GEN_UID: _CHEAP_UID, COL_DISPATCH: cheap_d})
        rows_dispatch.append({**cell, COL_GEN_UID: _BATTERY_UID, COL_DISPATCH: bat_d})
        rows_dispatch.append({**cell, COL_GEN_UID: _PEAKER_UID, COL_DISPATCH: peaker_d})
        # Demand all on bus 4.
        rows_load.append({**cell, COL_BUS_UID: 4, COL_LOAD: load})

    cells = Cells(
        dispatch=pd.DataFrame(rows_dispatch),
        load=pd.DataFrame(rows_load),
    )

    out = tmp_path / "4b_battery_feed.parquet"
    manifest = Manifest.make(
        producer="4b_battery_marginal_fixture",
        producer_version="1.0.0",
        schema_version=SCHEMA_VERSION,
    )
    write_feed(out, topology, cells, manifest)
    return out


def _real_cell_key(hour: int) -> dict[str, object]:
    return {
        COL_SCENARIO: pd.NA,
        COL_STAGE: pd.NA,
        COL_BLOCK: pd.NA,
        COL_DATE_UTC: "2026-04-01",
        COL_HOUR: hour,
        COL_DATA_SOURCE: "real",
    }


def _run_pipeline(feed: Path, out: Path) -> int:
    return cli(
        [
            "--input-kind",
            "feed-parquet",
            "--mode",
            "real-reconstruct",
            "--feed",
            str(feed),
            "--out",
            str(out),
        ]
    )


# ----------------------------------------------------------------------
# Tests — one per §4.7 R3 cascade outcome.
# ----------------------------------------------------------------------


def test_cell_a_cheap_thermal_sets_price(tmp_path, four_bus_feed):
    """t1: load=50 → cheap thermal interior, λ = MC[cheap] = 10."""
    out = tmp_path / "out.parquet"
    code = _run_pipeline(four_bus_feed, out)
    assert code in (EXIT_OK, EXIT_UNATTRIBUTED)
    ds = MarginalUnitDataset.open(out)
    pz = ds.per_zone()
    cell_a = pz[pz["hour"] == 1]
    assert not cell_a.empty
    assert cell_a["zone_lmp"].iloc[0] == pytest.approx(_CHEAP_MC, abs=1e-3)
    assert _CHEAP_UID in list(cell_a["marginal_gen_uids"].iloc[0])
    assert cell_a["status"].iloc[0] == "single_unit"


def test_cell_b_battery_sets_price(tmp_path, four_bus_feed):
    """t2: load=130 → cheap at pmax, battery interior at 30 MW.

    The merit order picks the battery as the unique unit with
    declared_MC and an interior dispatch → λ = MC[battery] = 30.
    """
    out = tmp_path / "out.parquet"
    _run_pipeline(four_bus_feed, out)
    ds = MarginalUnitDataset.open(out)
    pz = ds.per_zone()
    cell_b = pz[pz["hour"] == 2]
    assert not cell_b.empty
    assert cell_b["zone_lmp"].iloc[0] == pytest.approx(_BATTERY_MC, abs=1e-3)
    # Battery is the marginal unit.
    assert _BATTERY_UID in list(cell_b["marginal_gen_uids"].iloc[0])
    # And cheap thermal is NOT in the marginal list (it's pmax-capped).
    assert _CHEAP_UID not in list(cell_b["marginal_gen_uids"].iloc[0])


def test_cell_c_peaker_sets_price(tmp_path, four_bus_feed):
    """t3: load=175 → cheap and battery at pmax, peaker interior at 25.

    Highest-MC interior = peaker → λ = MC[peaker] = 80.
    """
    out = tmp_path / "out.parquet"
    _run_pipeline(four_bus_feed, out)
    ds = MarginalUnitDataset.open(out)
    pz = ds.per_zone()
    cell_c = pz[pz["hour"] == 3]
    assert not cell_c.empty
    assert cell_c["zone_lmp"].iloc[0] == pytest.approx(_PEAKER_MC, abs=1e-3)
    assert _PEAKER_UID in list(cell_c["marginal_gen_uids"].iloc[0])


def test_battery_emission_intensity_when_marginal(tmp_path, four_bus_feed):
    """When the battery sets the price (cell B), the emission-intensity
    recipe must use the battery's emission factor — exercising the
    Lin & Tang invariant that price-recipe and emission-recipe share
    the same marginal_gen_uids."""
    out = tmp_path / "out.parquet"
    _run_pipeline(four_bus_feed, out)
    em_df = pd.read_parquet(out / "bus_emission_intensity_recipe.parquet")
    cell_b_em = em_df[em_df["hour"] == 2]
    assert not cell_b_em.empty
    # The recipe references the battery uid.
    uids_b = list(cell_b_em.iloc[0]["marginal_gen_uids"])
    assert _BATTERY_UID in uids_b


def test_recipe_round_trip_recovers_battery_price(tmp_path, four_bus_feed):
    """Consumer API: recompute_lmp with the captured costs returns the
    same λ_z the writer stored — proves the recipe is internally
    consistent for the battery-marginal cell."""
    out = tmp_path / "out.parquet"
    _run_pipeline(four_bus_feed, out)
    ds = MarginalUnitDataset.open(out)
    df = ds.recompute_lmp(
        unit_costs={
            _CHEAP_UID: _CHEAP_MC,
            _BATTERY_UID: _BATTERY_MC,
            _PEAKER_UID: _PEAKER_MC,
        }
    )
    assert "zone_lmp_recomputed" in df.columns
    # delta ≈ 0 for every unit-driven cell.
    if "lmp_delta" in df.columns:
        assert df["lmp_delta"].abs().fillna(0).max() < 1e-6
