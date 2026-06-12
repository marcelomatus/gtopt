# SPDX-License-Identifier: BSD-3-Clause
"""Composite stress test — IEEE 57-bus topology + battery + solar +
24 hourly cells with realistic demand variation.

There is no IEEE-named battery case in the integration suite (the
seven IEEE benchmarks all have ``battery_array=[]``; battery
fixtures are 4-bus synthetic cases). This test composes the two
worlds:

  * IEEE 57-bus topology (57 buses, 80 lines, 7 thermals) loaded from
    the prebuilt ``test_output/ieee_57b/planning.json``.
  * 1 battery-shadow generator at a high-demand bus, parameters from
    ``bat_4b_24``'s ``battery_array``.
  * 3 solar-profile generators distributed across the network.
  * 24 hourly cells with a realistic demand profile (night-low →
    morning-ramp → midday-plateau → evening-peak → late-night-decline)
    and a bell-shaped solar profile (zero outside daylight, peaking
    near solar noon).
  * Thermal dispatch is allocated via **merit order** so the §4.7 R3
    cascade picks the highest-MC interior thermal as the price-setter
    and λ_z varies across the day.

The merged feed is written via ``gtopt_canonical_feed`` and analysed
with ``--input-kind feed-parquet --mode real-reconstruct``. The
24-hour evolution exercises:

  * §2.4 hydro/battery price-taker handling (kind="battery" → status
    HYDRO_MARGINAL when interior).
  * §4.4 row-8 ``profile_dispatched`` for solar units (never marginal).
  * §4.7 R3 merit-order pick on a real 7-thermal merit stack.
"""

from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from gtopt_canonical_feed import (
    SCHEMA_VERSION,
    Cells,
    Generator,
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
from gtopt_marginal_units._gtopt_reader import topology_from_planning
from gtopt_marginal_units.constants import EXIT_OK, EXIT_UNATTRIBUTED
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli


_OUTPUT_ROOT = Path("/home/marce/git/gtopt-hygiene/build/integration_test/test_output")
_IEEE57_PLANNING = _OUTPUT_ROOT / "ieee_57b" / "planning.json"
_BAT_PLANNING = _OUTPUT_ROOT / "bat_4b_24" / "planning.json"

# Where the synthetic generators land in the IEEE 57-bus topology.
_HOST_BUS_BATTERY = 8  # high-demand bus per the IEEE 57-bus standard
_HOST_BUSES_SOLAR = (12, 24, 42)
_BATTERY_GEN_UID = 10_000
_SOLAR_GEN_UIDS = (10_001, 10_002, 10_003)
_BATTERY_NAME = "bat_57b_synthetic"


# ----------------------------------------------------------------------
# Battery economic parameters — derived from the storage-economics
# literature, NOT made up.
#
# References:
# * Hittinger & Azevedo (2015), "Bulk Energy Storage Increases United
#   States Electricity System Emissions", Environ. Sci. Technol.
#   Storage charges from the marginal generator at low-price hours and
#   discharges during peaks. Net emissions per discharged MWh range
#   104-407 kg CO₂/MWh depending on grid mix.
# * Sioshansi, Denholm, Jenkin (2009), "Storage and the Value of
#   Electricity Pricing", The Energy Journal — opportunity-cost
#   framework: discharge MC = price-at-charge / round_trip_efficiency.
# * Lazard's Levelized Cost of Storage (LCOS) v8 (2024) — Li-ion
#   degradation cost ≈ $5-15 / MWh-discharged for 4-h batteries.
# * Round-trip efficiency η = 0.85 is mid-range for grid-scale Li-ion
#   (DOE Energy Storage Technology and Cost Characterization, 2019).
#
# Formula:
#   declared_MC[battery]    = LMP_at_charge / η + degradation_cost
#   emission_rate[battery] = EF_at_charge / η
# Charging assumption: the battery charges off-peak when the cheap
# thermals (MC=20, EF=400 kg CO₂/MWh in our synthetic catalogue) are
# marginal. v1 uses constants — a v1.1 multi-segment cost-curve model
# would resolve the cost per actual charging window.
# ----------------------------------------------------------------------
BATTERY_ROUND_TRIP_EFF = 0.85
BATTERY_LMP_AT_CHARGE = 20.0  # USD/MWh — cheap-thermal hours
BATTERY_DEGRADATION_COST = 5.0  # USD/MWh-discharged
BATTERY_EF_AT_CHARGE = 400.0  # kgCO₂/MWh — cheap-thermal EF
BATTERY_DECLARED_MC = (
    BATTERY_LMP_AT_CHARGE / BATTERY_ROUND_TRIP_EFF + BATTERY_DEGRADATION_COST
)  # ≈ 28.53 USD/MWh
BATTERY_EMISSION_FACTOR = BATTERY_EF_AT_CHARGE / BATTERY_ROUND_TRIP_EFF
# ≈ 470.59 kgCO₂/MWh (charges from cheap thermals; energy losses
# proportionally inflate the per-discharged-MWh emission intensity)

# IEEE 57b emission factors (synthetic — IEEE benchmarks don't ship
# emission_rate values). MC=20 thermals modelled as gas combined-cycle
# (≈400 kg CO₂/MWh), MC=40 thermals as oil/diesel peakers (≈700).
_IEEE57B_EF_BY_MC = {20.0: 400.0, 40.0: 700.0}


# ----------------------------------------------------------------------
# 24-hour profiles. Numbers are illustrative — chosen so the merit
# order surfaces a *time-varying* zone_lmp the test can assert on.
# ----------------------------------------------------------------------

# Demand (MW). Shape: night-low / morning-ramp / midday-plateau /
# evening-peak / late-decline.
#
# IEEE 57-bus thermals: cheap gens (uids 1/3/5/7 at MC=20) total
# 1675.88 MW pmax; peakers (uids 2/4/6 at MC=40) total 300 MW. The
# numbers below are scaled so peakers come on at the evening peak
# (h=17,18,19) — pushing λ_z = 40 — while every other hour stays
# inside the cheap-gen ceiling (λ_z = 20). This produces visible
# time-of-day variation in the merit-order pick.
DEMAND_PROFILE = (
    600.0,
    580.0,
    540.0,
    540.0,
    580.0,
    660.0,  # 0-5  night
    850.0,
    1000.0,
    1180.0,
    1260.0,
    1290.0,
    1280.0,  # 6-11 morning ramp
    1270.0,
    1260.0,
    1240.0,
    1280.0,
    1600.0,
    2000.0,  # 12-17 noon → peak
    2000.0,
    1900.0,
    1500.0,
    1100.0,
    850.0,
    700.0,  # 18-23 evening → late
)

# Combined solar profile (MW). Zero outside daylight, bell-shaped at noon.
SOLAR_PROFILE = (
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,  # 0-5  night
    50.0,
    200.0,
    400.0,
    550.0,
    650.0,
    700.0,  # 6-11 morning ramp
    700.0,
    650.0,
    550.0,
    400.0,
    200.0,
    50.0,  # 12-17 afternoon decline
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,  # 18-23 night
)

# Battery discharge schedule (MW > 0 = discharging into the grid).
# Dispatches at half pmax during the evening peak when solar is gone.
_BATTERY_PEAK_HOURS = range(17, 22)


# ----------------------------------------------------------------------
# Fixture: build a canonical feed and return its path.
# ----------------------------------------------------------------------


@pytest.fixture
def composite_feed(tmp_path: Path) -> Path:
    if not _IEEE57_PLANNING.exists() or not _BAT_PLANNING.exists():
        pytest.skip("IEEE 57b or bat_4b_24 prebuilt output not available")

    ieee57 = json.loads(_IEEE57_PLANNING.read_text(encoding="utf-8"))
    bat_planning = json.loads(_BAT_PLANNING.read_text(encoding="utf-8"))

    base_topo = topology_from_planning(ieee57)
    bat_params = bat_planning["system"]["battery_array"][0]

    # Inject synthetic emission factors on the IEEE 57b thermals so the
    # emission-intensity recipe table has data to work with. (IEEE
    # benchmarks don't ship emission_rate values.)
    base_thermals = [
        Generator(
            uid=g.uid,
            name=g.name,
            bus_uid=g.bus_uid,
            pmin=g.pmin,
            pmax=g.pmax,
            declared_MC=g.declared_MC,
            kind=g.kind,
            emission_rate=_IEEE57B_EF_BY_MC.get(
                float(g.declared_MC) if g.declared_MC is not None else -1.0,
                500.0,  # fallback for any thermal not at MC=20 or 40
            ),
        )
        for g in base_topo.generators
    ]

    # Battery at the chosen high-demand bus, with declared_MC and
    # emission_rate derived from the storage-economics literature
    # (see module-level constants).
    battery_gen = Generator(
        uid=_BATTERY_GEN_UID,
        name=_BATTERY_NAME,
        bus_uid=_HOST_BUS_BATTERY,
        pmin=0.0,
        pmax=float(bat_params["pmax_discharge"]),  # 60 MW from bat_4b_24
        declared_MC=BATTERY_DECLARED_MC,  # ≈ 28.53 USD/MWh
        kind="battery",
        emission_rate=BATTERY_EMISSION_FACTOR,  # ≈ 470.59 kgCO₂/MWh
    )

    # Three solar-profile generators distributed across the network.
    solar_gens = [
        Generator(
            uid=uid,
            name=f"solar_{i}",
            bus_uid=bus_uid,
            pmin=0.0,
            pmax=120.0,  # ~360 MW combined, capped by SOLAR_PROFILE
            declared_MC=0.0,
            kind="profile",
            emission_rate=0.0,
        )
        for i, (uid, bus_uid) in enumerate(zip(_SOLAR_GEN_UIDS, _HOST_BUSES_SOLAR))
    ]

    topology = Topology(
        buses=list(base_topo.buses),
        generators=base_thermals + [battery_gen] + solar_gens,
        lines=list(base_topo.lines),
    )

    # Sort thermals by (MC ascending, uid ascending) for merit-order dispatch.
    thermals_sorted = sorted(
        base_thermals,
        key=lambda g: (
            float(g.declared_MC) if g.declared_MC is not None else float("inf"),
            g.uid,
        ),
    )
    bus_uids = [b.uid for b in base_topo.buses]

    rows_dispatch: list[dict] = []
    rows_load: list[dict] = []

    for hour in range(24):
        cell_key = _real_cell_key(hour)
        demand = DEMAND_PROFILE[hour]
        solar_total = SOLAR_PROFILE[hour]
        battery_dispatch = (
            battery_gen.pmax * 0.5 if hour in _BATTERY_PEAK_HOURS else 0.0
        )

        # Residual after non-thermal supply, allocated to thermals in
        # merit order until the residual is met.
        residual = max(0.0, demand - solar_total - battery_dispatch)
        thermal_dispatch: dict[int, float] = {g.uid: 0.0 for g in thermals_sorted}
        remaining = residual
        for g in thermals_sorted:
            allocate = min(remaining, g.pmax)
            thermal_dispatch[g.uid] = allocate
            remaining -= allocate
            if remaining <= 1.0e-9:
                break

        # Emit dispatch rows.
        for uid, dispatch in thermal_dispatch.items():
            rows_dispatch.append({**cell_key, COL_GEN_UID: uid, COL_DISPATCH: dispatch})
        rows_dispatch.append(
            {**cell_key, COL_GEN_UID: battery_gen.uid, COL_DISPATCH: battery_dispatch}
        )
        # Solar split equally across the three plants.
        per_solar = solar_total / len(solar_gens) if solar_total > 0 else 0.0
        for s in solar_gens:
            rows_dispatch.append(
                {**cell_key, COL_GEN_UID: s.uid, COL_DISPATCH: per_solar}
            )

        # Load is distributed evenly across all buses (smoke fixture —
        # we don't need a faithful OPF).
        total_supply = sum(thermal_dispatch.values()) + battery_dispatch + solar_total
        per_bus = total_supply / len(bus_uids)
        for u in bus_uids:
            rows_load.append({**cell_key, COL_BUS_UID: u, COL_LOAD: per_bus})

    cells = Cells(
        dispatch=pd.DataFrame(rows_dispatch),
        load=pd.DataFrame(rows_load),
    )

    out = tmp_path / "ieee_57b_with_bat_solar_feed.parquet"
    manifest = Manifest.make(
        producer="ieee_57b_with_bat_solar_fixture",
        producer_version="1.0.0",
        schema_version=SCHEMA_VERSION,
        extras={
            "host_topology": "ieee_57b",
            "battery_source": "bat_4b_24",
            "battery_host_bus": _HOST_BUS_BATTERY,
            "solar_host_buses": list(_HOST_BUSES_SOLAR),
        },
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


# ----------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------


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


def test_composite_runs_to_completion(tmp_path, composite_feed):
    """Pipeline handles 57 buses + 1 battery + 3 solar + 24 cells."""
    out = tmp_path / "composite_marginals.parquet"
    code = _run_pipeline(composite_feed, out)
    assert code in (EXIT_OK, EXIT_UNATTRIBUTED)

    ds = MarginalUnitDataset.open(out)
    assert ds.per_zone()["hour"].nunique() == 24


def test_composite_battery_appears_at_host_bus(tmp_path, composite_feed):
    """The battery shadow-generator shows up at its host bus across 24 cells."""
    out = tmp_path / "composite_marginals.parquet"
    _run_pipeline(composite_feed, out)
    ds = MarginalUnitDataset.open(out)
    bat_rows = ds.per_bus()[ds.per_bus()["gen_uid"] == _BATTERY_GEN_UID]
    assert len(bat_rows) == 24
    assert (bat_rows["bus_uid"] == _HOST_BUS_BATTERY).all()


def test_battery_24h_time_evolution(tmp_path, composite_feed):
    """Battery transitions OFF at night → HYDRO_MARGINAL at evening peak."""
    out = tmp_path / "composite_marginals.parquet"
    _run_pipeline(composite_feed, out)
    ds = MarginalUnitDataset.open(out)
    bat = (
        ds.per_bus()[ds.per_bus()["gen_uid"] == _BATTERY_GEN_UID]
        .sort_values("hour")
        .reset_index(drop=True)
    )

    peak_hours = list(_BATTERY_PEAK_HOURS)
    for _, row in bat.iterrows():
        if int(row["hour"]) in peak_hours:
            assert row["status"] == "hydro_marginal"
            assert row["dispatch"] == pytest.approx(30.0)  # pmax/2 = 60/2
        else:
            assert row["status"] == "off"
            assert row["dispatch"] == 0.0


def test_solar_units_classified_as_profile_dispatched(tmp_path, composite_feed):
    """Solar (kind=profile) must NEVER be marginal, even at zero LMP.

    Master §4.4 row 8: profile_dispatched is its own status; profile
    units are filtered out of the merit ladder by §4.9.1.
    """
    out = tmp_path / "composite_marginals.parquet"
    _run_pipeline(composite_feed, out)
    ds = MarginalUnitDataset.open(out)
    per_bus = ds.per_bus()
    solar_rows = per_bus[per_bus["gen_uid"].isin(_SOLAR_GEN_UIDS)]

    # Each solar plant is observed in 24 hourly rows.
    assert len(solar_rows) == 24 * len(_SOLAR_GEN_UIDS)

    # Status must be either "off" (night, dispatch=0) or
    # "profile_dispatched" (any positive dispatch).
    statuses = set(solar_rows["status"].dropna().unique())
    assert statuses <= {"off", "profile_dispatched"}, (
        f"unexpected solar status: {statuses}"
    )

    # Solar must never be flagged as marginal.
    assert not solar_rows["is_marginal"].any()


def test_zone_lmp_three_tier_evolution(tmp_path, composite_feed):
    """Demand variation + solar contribution + battery discharge cause
    the merit-order pick to land on three distinct price tiers across
    the 24-hour day:

    * λ = 20 USD/MWh — cheap-thermal hours (most of the day; solar
      shaves the morning, low load at night).
    * λ ≈ 28.5 USD/MWh — battery-marginal hours (post-peak when only
      a cheap thermal and the battery are interior; the battery's
      derived MC is the merit-order ceiling).
    * λ = 40 USD/MWh — peaker hours (evening peak, residual exceeds
      cheap-thermal capacity 1675.88 MW).
    """
    out = tmp_path / "composite_marginals.parquet"
    _run_pipeline(composite_feed, out)
    ds = MarginalUnitDataset.open(out)
    pz = ds.per_zone().sort_values("hour").reset_index(drop=True)
    assert len(pz) == 24

    # All values land in the legal range [0, demand_fail_cost].
    assert (pz["zone_lmp"] >= 0).all()
    assert (pz["zone_lmp"] <= 1000.0).all()

    lmps = sorted(pz["zone_lmp"].round(3).unique().tolist())
    # Expect three distinct price tiers.
    assert len(lmps) == 3, f"expected 3 price tiers across 24h; got {lmps}"
    assert lmps[0] == pytest.approx(20.0, abs=1e-3), f"cheap-tier λ: {lmps[0]}"
    assert lmps[1] == pytest.approx(BATTERY_DECLARED_MC, abs=1e-3), (
        f"battery-tier λ: {lmps[1]} (expected ≈{BATTERY_DECLARED_MC:.2f})"
    )
    assert lmps[2] == pytest.approx(40.0, abs=1e-3), f"peaker-tier λ: {lmps[2]}"


def test_battery_sets_price_post_peak(tmp_path, composite_feed):
    """Find a cell where the battery is the price-setter — i.e. the
    formula_kind is single_unit/tied_units anchored on the battery
    UID and λ_z equals the derived BATTERY_DECLARED_MC.

    This exercises the master §4.7 R3 path where a hydro/battery unit
    with a non-NULL declared_MC participates in the merit-order pick
    on equal footing with thermals.
    """
    out = tmp_path / "composite_marginals.parquet"
    _run_pipeline(composite_feed, out)
    ds = MarginalUnitDataset.open(out)
    pz = ds.per_zone()
    # Cells whose marginal_gen_uids list contains the battery.
    bat_set_price = pz[
        pz["marginal_gen_uids"].apply(
            lambda lst: lst is not None and _BATTERY_GEN_UID in list(lst)
        )
    ]
    assert not bat_set_price.empty, (
        "expected at least one cell where the battery sets the price"
    )
    # And at those cells the zone_lmp must match the battery's MC.
    assert ((bat_set_price["zone_lmp"] - BATTERY_DECLARED_MC).abs() < 1e-3).all()


def test_solar_dispatch_follows_profile(tmp_path, composite_feed):
    """Solar dispatch must mirror SOLAR_PROFILE / 3 (split across 3 plants)."""
    out = tmp_path / "composite_marginals.parquet"
    _run_pipeline(composite_feed, out)
    ds = MarginalUnitDataset.open(out)
    per_bus = ds.per_bus()
    # Pick one solar plant and check its 24-h dispatch profile.
    solar_a = (
        per_bus[per_bus["gen_uid"] == _SOLAR_GEN_UIDS[0]]
        .sort_values("hour")
        .reset_index(drop=True)
    )
    expected_per_plant = [v / len(_SOLAR_GEN_UIDS) for v in SOLAR_PROFILE]
    for h, expected in enumerate(expected_per_plant):
        row = solar_a[solar_a["hour"] == h].iloc[0]
        assert row["dispatch"] == pytest.approx(expected, abs=1e-3), (
            f"hour {h}: solar dispatch {row['dispatch']} != expected {expected}"
        )


def test_consumer_api_smoke_at_scale(tmp_path, composite_feed):
    """Consumer-API methods run on the composite dataset."""
    out = tmp_path / "composite_marginals.parquet"
    _run_pipeline(composite_feed, out)
    ds = MarginalUnitDataset.open(out)
    assert not ds.bus_lmp().empty
    # Combined dual-currency view loads at scale.
    assert not ds.bus_lmp_and_emission().empty
    # outage_sensitivity returns at most 24 rows for any single thermal.
    if ds.merit_ladder().shape[0] > 0:
        # If any unit was rank-0 marginal, outage_sensitivity returns
        # one row per hour where it was marginal.
        for _, row in ds.merit_ladder().head(1).iterrows():
            uid = row.get("gen_uid")
            if pd.notna(uid):
                _ = ds.outage_sensitivity(int(uid))
                break
