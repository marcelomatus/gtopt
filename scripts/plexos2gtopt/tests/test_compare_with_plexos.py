"""Unit tests for ``plexos2gtopt.compare_with_plexos``.

The comparison tool shells out to ``mdb-export`` to read PLEXOS .accdb
solution tables and to ``zstd`` for the cache layer.  These tests mock
``subprocess.check_output`` and ``subprocess.run`` so the tests can
exercise the pure-Python aggregation / classification logic without
requiring either the binary or a real .accdb on disk.

Locks in the demand-scope decision (Node.Load not Region.Load) so a
future refactor can't silently re-introduce the phantom 7-8% demand
gap that was caught and fixed in fd75f6d14.
"""

from __future__ import annotations

import io
import json
from pathlib import Path
from unittest import mock

import pytest

from plexos2gtopt.compare_with_plexos import (
    PLEXOS_PROP_BATTERY_LOAD,
    PLEXOS_PROP_GENERATOR_GENCOST,
    PLEXOS_PROP_GENERATOR_GENERATION,
    PLEXOS_PROP_GENERATOR_SRMC,
    PLEXOS_PROP_NODE_LOAD,
    PLEXOS_PROP_REGION_GENERATION,
    PLEXOS_PROP_REGION_LOAD,
    PLEXOS_PROP_REGION_LOSSES,
    _classify_tech,
    _plexos_block_durations_from_accdb,
    compute_gtopt_energy_totals,
    compute_gtopt_generation_by_technology,
    compute_plexos_energy_totals,
)


# ----------------------------------------------------------------
# _classify_tech — pure name → bucket mapping
# ----------------------------------------------------------------


@pytest.mark.parametrize(
    "category,expected",
    [
        ("Hydro Gen Group A", "hydro"),
        ("Hydro Gen Group C", "hydro"),
        ("Solar Farms", "solar"),
        ("Wind Farms", "wind"),
        ("Thermal Gen N. Zone", "thermal"),
        ("Thermal Gen S. Zone", "thermal"),
        ("Termicas Ficticias", "thermal"),
        ("Hidroelectricas", "hydro"),  # Spanish spelling
        ("", "other"),
        ("Storages - Maule", "other"),
    ],
)
def test_classify_tech_buckets(category: str, expected: str) -> None:
    assert _classify_tech(category) == expected


# ----------------------------------------------------------------
# Mock fixtures for PLEXOS .accdb extraction.
# ----------------------------------------------------------------


def _mock_table(rows: list[dict[str, str]]) -> str:
    """Build a CSV blob that mimics ``mdb-export``'s output."""
    if not rows:
        return ""
    headers = list(rows[0].keys())
    out = io.StringIO()
    out.write(",".join(headers) + "\n")
    for r in rows:
        out.write(",".join(str(r[h]) for h in headers) + "\n")
    return out.getvalue()


@pytest.fixture
def mock_accdb_tables() -> dict[str, list[dict[str, str]]]:
    """Synthetic PLEXOS solution-database tables.

    Schema: two regions, two nodes per region, two generators (one
    thermal, one hydro), one battery, one storage, two periods with
    durations 2 h and 3 h (total 5 h horizon).
    """
    t_phase_3 = [
        {"period_id": "1", "interval_id": "1"},
        {"period_id": "1", "interval_id": "2"},  # period 1 = 2 h
        {"period_id": "2", "interval_id": "3"},
        {"period_id": "2", "interval_id": "4"},
        {"period_id": "2", "interval_id": "5"},  # period 2 = 3 h
    ]
    t_object = [
        # class_id 2 = Generator, 7 = Battery, 8 = Storage, 22 = Node, 24 = Line
        {
            "object_id": "1",
            "class_id": "2",
            "name": "GEN_THERMAL",
            "category_id": "100",
        },
        {"object_id": "2", "class_id": "2", "name": "GEN_HYDRO", "category_id": "97"},
        {"object_id": "3", "class_id": "22", "name": "BUS_NORTH", "category_id": "0"},
        {"object_id": "4", "class_id": "22", "name": "BUS_SOUTH", "category_id": "0"},
        {"object_id": "5", "class_id": "200", "name": "REGION_A", "category_id": "0"},
    ]
    t_category = [
        {"category_id": "97", "name": "Hydro Gen Group A"},
        {"category_id": "100", "name": "Thermal Gen N. Zone"},
    ]
    t_membership = [
        # PLEXOS uses one membership per (system, child) pair for the
        # primary collection.  For our mock we keep it minimal.
        {"membership_id": "10", "child_object_id": "1"},  # GEN_THERMAL
        {"membership_id": "11", "child_object_id": "2"},  # GEN_HYDRO
        {"membership_id": "20", "child_object_id": "3"},  # BUS_NORTH
        {"membership_id": "21", "child_object_id": "4"},  # BUS_SOUTH
        {"membership_id": "30", "child_object_id": "5"},  # REGION_A
    ]
    t_key = [
        # Generator GENERATION (prop 2) — thermal at 100 MW, hydro at 50 MW
        {
            "key_id": "1",
            "membership_id": "10",
            "property_id": str(PLEXOS_PROP_GENERATOR_GENERATION),
        },
        {
            "key_id": "2",
            "membership_id": "11",
            "property_id": str(PLEXOS_PROP_GENERATOR_GENERATION),
        },
        # Generator SRMC (prop 137) — thermal $50, hydro $0
        {
            "key_id": "3",
            "membership_id": "10",
            "property_id": str(PLEXOS_PROP_GENERATOR_SRMC),
        },
        {
            "key_id": "4",
            "membership_id": "11",
            "property_id": str(PLEXOS_PROP_GENERATOR_SRMC),
        },
        # Generator GenCost (prop 119) — thermal contributes $X per block
        {
            "key_id": "5",
            "membership_id": "10",
            "property_id": str(PLEXOS_PROP_GENERATOR_GENCOST),
        },
        # NODE.Load (prop 1373) — consumer demand on BUS_NORTH=80, BUS_SOUTH=70
        {
            "key_id": "6",
            "membership_id": "20",
            "property_id": str(PLEXOS_PROP_NODE_LOAD),
        },
        {
            "key_id": "7",
            "membership_id": "21",
            "property_id": str(PLEXOS_PROP_NODE_LOAD),
        },
        # REGION.Load (prop 966) — gross 160 (= 150 demand + 10 battery)
        {
            "key_id": "8",
            "membership_id": "30",
            "property_id": str(PLEXOS_PROP_REGION_LOAD),
        },
        # REGION.Generation (prop 970)
        {
            "key_id": "9",
            "membership_id": "30",
            "property_id": str(PLEXOS_PROP_REGION_GENERATION),
        },
        # Battery.Load (prop 521)
        {
            "key_id": "10",
            "membership_id": "30",
            "property_id": str(PLEXOS_PROP_BATTERY_LOAD),
        },
        # Region Losses (prop 997)
        {
            "key_id": "11",
            "membership_id": "30",
            "property_id": str(PLEXOS_PROP_REGION_LOSSES),
        },
    ]
    t_data_0 = [
        # Generator generation (MW per period)
        {"key_id": "1", "period_id": "1", "value": "100"},
        {"key_id": "1", "period_id": "2", "value": "100"},
        {"key_id": "2", "period_id": "1", "value": "50"},
        {"key_id": "2", "period_id": "2", "value": "50"},
        # SRMC
        {"key_id": "3", "period_id": "1", "value": "50"},
        {"key_id": "3", "period_id": "2", "value": "50"},
        {"key_id": "4", "period_id": "1", "value": "0"},
        {"key_id": "4", "period_id": "2", "value": "0"},
        # Gen cost: thermal 100 MW × 50 $/MWh × 2 h = 10,000 (period 1)
        #          thermal 100 MW × 50 $/MWh × 3 h = 15,000 (period 2)
        # but PLEXOS prop 119 ships pre-integrated block $:
        {"key_id": "5", "period_id": "1", "value": "10000"},
        {"key_id": "5", "period_id": "2", "value": "15000"},
        # Node.Load
        {"key_id": "6", "period_id": "1", "value": "80"},  # BUS_NORTH 80 MW
        {"key_id": "6", "period_id": "2", "value": "80"},
        {"key_id": "7", "period_id": "1", "value": "70"},  # BUS_SOUTH 70 MW
        {"key_id": "7", "period_id": "2", "value": "70"},
        # Region.Load (gross = 160 includes batt charging)
        {"key_id": "8", "period_id": "1", "value": "160"},
        {"key_id": "8", "period_id": "2", "value": "160"},
        # Region.Generation
        {"key_id": "9", "period_id": "1", "value": "150"},
        {"key_id": "9", "period_id": "2", "value": "150"},
        # Battery.Load (charging = 10 MW)
        {"key_id": "10", "period_id": "1", "value": "10"},
        {"key_id": "10", "period_id": "2", "value": "10"},
        # Losses (0 in this toy)
        {"key_id": "11", "period_id": "1", "value": "0"},
        {"key_id": "11", "period_id": "2", "value": "0"},
    ]
    return {
        "t_phase_3": t_phase_3,
        "t_object": t_object,
        "t_category": t_category,
        "t_membership": t_membership,
        "t_key": t_key,
        "t_data_0": t_data_0,
    }


@pytest.fixture
def mock_mdb_export(monkeypatch, mock_accdb_tables):
    """Patch ``subprocess.check_output`` and ``subprocess.run`` so any
    ``mdb-export <accdb> <table>`` call returns our synthetic CSV
    instead of shelling out to the real binary."""

    def fake_check_output(cmd, **kwargs):
        # cmd is ["mdb-export", "<accdb>", "<table>"]
        assert cmd[0] == "mdb-export"
        table = cmd[2]
        rows = mock_accdb_tables.get(table, [])
        return _mock_table(rows)

    def fake_run(cmd, **kwargs):
        if cmd and cmd[0] == "mdb-export":
            table = cmd[2]
            rows = mock_accdb_tables.get(table, [])
            return mock.Mock(stdout=_mock_table(rows), returncode=0)
        raise NotImplementedError(f"unmocked subprocess: {cmd!r}")

    monkeypatch.setattr("subprocess.check_output", fake_check_output)
    monkeypatch.setattr("subprocess.run", fake_run)


# ----------------------------------------------------------------
# _plexos_block_durations_from_accdb
# ----------------------------------------------------------------


def test_block_durations_from_t_phase_3(tmp_path: Path, mock_mdb_export) -> None:
    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.touch()
    dur = _plexos_block_durations_from_accdb(fake_accdb)
    assert dur == {1: 2, 2: 3}  # period 1 = 2 intervals, period 2 = 3 intervals
    assert sum(dur.values()) == 5


# ----------------------------------------------------------------
# compute_plexos_energy_totals
# ----------------------------------------------------------------


def test_compute_plexos_energy_totals_uses_node_load_not_region(
    tmp_path: Path, mock_mdb_export
) -> None:
    """LOCKED: ``load_mwh`` returns Node.Load − Battery.Load (true
    consumer demand), NOT Region.Load (gross) or Node.Load alone
    (gross of battery charging).

    Node.Load (prop 1373) includes the battery's charging draw at
    the node where the battery sits; gtopt's filtered Demand
    (consumer-only, by-input-bundle-uid) is consumer-only.
    Subtracting Battery.Load (prop 521) makes both sides
    apples-to-apples consumer demand.  The raw values are still
    published separately as ``load_node_mwh`` and
    ``battery_load_mwh`` for downstream tools.
    """
    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.touch()
    tot = compute_plexos_energy_totals(fake_accdb)

    # Node.Load = (80 + 70) MW × (2 + 3) h = 150 MW × 5 h = 750 MWh
    # Battery.Load = 10 MW × 5 h = 50 MWh
    # load_mwh (consumer-only) = Node.Load - Battery.Load = 700 MWh
    assert tot["load_mwh"] == pytest.approx(700)
    assert tot["load_node_mwh"] == pytest.approx(750)
    # Region.Load = 160 MW × 5 h = 800 MWh — published separately
    assert tot["load_region_mwh"] == pytest.approx(800)
    # The difference IS Battery.Load + Losses (10 MW × 5 h + 0)
    assert tot["battery_load_mwh"] == pytest.approx(50)
    assert tot["losses_mwh"] == pytest.approx(0)
    # And Region.Load - Node.Load == Battery.Load + Losses
    assert tot["load_region_mwh"] - tot["load_node_mwh"] == pytest.approx(
        tot["battery_load_mwh"] + tot["losses_mwh"]
    )

    # Generation cost: prop 119 is per-block $ (no duration multiplier).
    # Thermal: 10,000 (period 1) + 15,000 (period 2) = 25,000.
    assert tot["gen_cost_usd"] == pytest.approx(25_000)

    # Block layout metadata.
    assert tot["block_count"] == 2
    assert tot["hours_covered"] == 5


# ----------------------------------------------------------------
# compute_gtopt_energy_totals — pure parquet read, no PLEXOS dep
# ----------------------------------------------------------------


def _write_gtopt_case(case_dir: Path) -> None:
    """Write a minimal gtopt case dir: bundle JSON + Demand/load_sol +
    Demand/fail_sol + Generator/generation_sol parquets."""
    case_dir.mkdir(parents=True, exist_ok=True)
    bundle = {
        "simulation": {
            "block_array": [
                {"uid": 1, "duration": 2.0},
                {"uid": 2, "duration": 3.0},
            ],
            "stage_array": [{"uid": 1, "first_block": 0, "count_block": 2}],
            "scenario_array": [{"uid": 1, "probability_factor": 1.0}],
        },
        "system": {
            "generator_array": [
                {"uid": 1, "name": "GEN_THERMAL"},
                {"uid": 2, "name": "GEN_HYDRO"},
            ],
            "demand_array": [
                {"uid": 1, "name": "load_north", "bus": "BUS_NORTH"},
                {"uid": 2, "name": "load_south", "bus": "BUS_SOUTH"},
            ],
            "bus_array": [
                {"uid": 1, "name": "BUS_NORTH"},
                {"uid": 2, "name": "BUS_SOUTH"},
            ],
        },
    }
    (case_dir / "bundle.json").write_text(json.dumps(bundle))

    import pyarrow as pa
    import pyarrow.parquet as pq

    def _write(rel: str, rows: list[dict[str, object]]) -> None:
        path = case_dir / "output" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        tbl = pa.Table.from_pylist(rows)
        pq.write_table(tbl, path)

    # Generator dispatch (MW per (uid, block))
    _write(
        "Generator/generation_sol.parquet",
        [
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 1,
                "uid": 1,
                "value": 100.0,
            },
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 2,
                "uid": 1,
                "value": 100.0,
            },
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 1,
                "uid": 2,
                "value": 50.0,
            },
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 2,
                "uid": 2,
                "value": 50.0,
            },
        ],
    )
    # Demand load served
    _write(
        "Demand/load_sol.parquet",
        [
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 1,
                "uid": 1,
                "value": 80.0,
            },
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 2,
                "uid": 1,
                "value": 80.0,
            },
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 1,
                "uid": 2,
                "value": 70.0,
            },
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 2,
                "uid": 2,
                "value": 70.0,
            },
        ],
    )
    # Demand fail (zero)
    _write(
        "Demand/fail_sol.parquet",
        [
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 1,
                "uid": 1,
                "value": 0.0,
            },
        ],
    )


def test_compute_gtopt_energy_totals_integrates_block_duration(
    tmp_path: Path,
) -> None:
    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    tot = compute_gtopt_energy_totals(case_dir)

    # Demand: (80 + 70) MW × (2 + 3) h = 750 MWh — matches Node.Load.
    assert tot["load_mwh"] == pytest.approx(750)
    # Gen: (100 + 50) MW × 5 h = 750 MWh — balances demand.
    assert tot["gen_mwh"] == pytest.approx(750)
    assert tot["fail_mwh"] == pytest.approx(0)
    assert tot["block_count"] == 2
    assert tot["hours_covered"] == 5


def test_gtopt_demand_matches_plexos_node_load(tmp_path: Path, mock_mdb_export) -> None:
    """End-to-end apples-to-apples — gtopt demand == PLEXOS
    (Node.Load - Battery.Load) on the synthetic case where both
    sides see the same load profile.

    gtopt's ``load_mwh`` is consumer-only (synthetic ``<bat>_dem``
    rows in ``Demand/load_sol.parquet`` are filtered out by
    ``_sum_consumer_demand_field_mwh``); PLEXOS's ``load_mwh`` is
    now also consumer-only (Node.Load gross of battery charging,
    minus Battery.Load) — apples-to-apples.
    """
    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    g = compute_gtopt_energy_totals(case_dir)

    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.touch()
    p = compute_plexos_energy_totals(fake_accdb)

    # The synthetic gtopt fixture has no Battery rows, so gtopt's
    # filtered ``load_mwh`` equals raw Node.Load.  PLEXOS's
    # ``load_mwh`` here subtracts the mock's 50-MWh Battery.Load
    # to produce 700 — so the apples-to-apples comparison
    # uses ``load_node_mwh`` (the pre-subtraction Node.Load = 750).
    assert g["load_mwh"] == pytest.approx(p["load_node_mwh"]), (
        "gtopt consumer demand must equal PLEXOS Node.Load (gross at "
        "the node) when gtopt has no batteries; if this fails, check "
        "compute_plexos_energy_totals' 'load_node_mwh' key is reading "
        "prop 1373."
    )


# ----------------------------------------------------------------
# Technology classification + generation breakdown
# ----------------------------------------------------------------


def test_compute_gtopt_generation_by_technology_with_plexos_categories(
    tmp_path: Path, mock_mdb_export
) -> None:
    """When an .accdb is supplied, units are classified via PLEXOS's
    t_category (GEN_THERMAL → 'thermal', GEN_HYDRO → 'hydro')."""
    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.touch()
    tech = compute_gtopt_generation_by_technology(case_dir, accdb_path=fake_accdb)
    # Each generates 5 h × MW = 500 (thermal) / 250 (hydro).
    assert tech == pytest.approx({"thermal": 500.0, "hydro": 250.0})


def test_compute_gtopt_generation_by_technology_heuristic_fallback(
    tmp_path: Path,
) -> None:
    """Without an .accdb, fall back to the name-pattern heuristic
    (BAT_*, _FV, _EO, _U[N], else thermal)."""
    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    # No accdb_path passed → all generators classify as 'thermal' by
    # the fallback heuristic (since GEN_THERMAL / GEN_HYDRO don't
    # match BAT_* / _FV / _EO / _U[N]).
    tech = compute_gtopt_generation_by_technology(case_dir, accdb_path=None)
    assert tech == pytest.approx({"thermal": 750.0})
