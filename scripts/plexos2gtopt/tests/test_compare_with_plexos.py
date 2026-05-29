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
    PLEXOS_PROP_BATTERY_CHARGING,
    PLEXOS_PROP_BATTERY_DISCHARGING,
    PLEXOS_PROP_BATTERY_GENERATION,
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
    compute_plexos_battery_operation,
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
    # No synthetic battery companions in this fixture → all gen is real.
    assert tot["gen_real_mwh"] == pytest.approx(750)
    assert tot["gen_synth_bat_mwh"] == pytest.approx(0)
    # No flow parquets, no batteries, no extras → all three loss measures = 0,
    # primary "losses_mwh" falls back to the bus residual.
    assert tot["losses_line_extras_mwh"] == pytest.approx(0)
    assert tot["losses_line_analytic_mwh"] == pytest.approx(0)
    assert tot["losses_bus_residual_mwh"] == pytest.approx(0)
    assert tot["losses_mwh"] == pytest.approx(0)
    assert tot["bes_round_trip_mwh"] == pytest.approx(0)
    assert tot["fail_mwh"] == pytest.approx(0)
    assert tot["block_count"] == 2
    assert tot["hours_covered"] == 5


def test_compute_gtopt_energy_totals_splits_synthetic_battery_gen(
    tmp_path: Path,
) -> None:
    """Generator/generation_sol rows whose uid is NOT in the bundle's
    ``generator_array`` are the synthetic ``<bat>_gen`` companions that
    gtopt's ``expand_batteries`` injects at LP-build time.  The energy-
    totals helper must split them out so the bus residual is expressible
    in real-bus terms.  Regression for the 2026-05-28 losses audit
    (``losses_audit.md``): without the split, ``gen_total`` doubles-up
    battery discharge against ``Battery/fout_sol`` and the loss residual
    drifts.
    """
    import pyarrow as pa
    import pyarrow.parquet as pq

    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    # Append a synthetic gen row whose uid (999) is NOT in
    # generator_array — emulates a ``<bat>_gen`` companion.
    base = pq.read_table(
        case_dir / "output" / "Generator" / "generation_sol.parquet"
    ).to_pandas()
    extra = pa.Table.from_pylist(
        [
            {
                "scene": 0,
                "phase": 0,
                "scenario": 1,
                "stage": 1,
                "block": 1,
                "uid": 999,
                "value": 25.0,
            },
        ]
    )
    pq.write_table(
        pa.concat_tables([pa.Table.from_pandas(base), extra]),
        case_dir / "output" / "Generator" / "generation_sol.parquet",
    )
    tot = compute_gtopt_energy_totals(case_dir)
    # Real gen unchanged (750 MWh), synth bumps gen_total by 25 MW × 2h.
    assert tot["gen_real_mwh"] == pytest.approx(750)
    assert tot["gen_synth_bat_mwh"] == pytest.approx(50)
    assert tot["gen_mwh"] == pytest.approx(800)


def test_compute_gtopt_energy_totals_analytic_line_losses(
    tmp_path: Path,
) -> None:
    """Analytic loss = ``Σ (R/V²)·|f|²·dur`` from Line/flowp_sol +
    Line/flown_sol; aliased to ``losses_mwh`` when available.

    Fixture: one line with R=0.04, V=1.0, carrying +100 MW for 2 h and
    −50 MW for 3 h → ``0.04 × (100² × 2 + 50² × 3) = 0.04 × 27500 =
    1100 MWh``.  The flowp/flown convention is disjoint (positive
    and negative directions reported in separate columns), so the
    absolute flow per block is ``flowp + flown``.
    """
    import pyarrow as pa
    import pyarrow.parquet as pq

    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    # Extend the bundle with one line carrying R=0.04 p.u., V=1.0.
    bundle_path = case_dir / "bundle.json"
    bundle = json.loads(bundle_path.read_text())
    bundle["system"]["line_array"] = [
        {"uid": 10, "name": "L1", "resistance": 0.04, "voltage": 1.0},
    ]
    bundle_path.write_text(json.dumps(bundle))
    out = case_dir / "output" / "Line"
    out.mkdir(parents=True, exist_ok=True)
    # flowp: block 1 → 100 MW forward; block 2 → 0
    pq.write_table(
        pa.Table.from_pylist(
            [
                {
                    "scene": 0,
                    "phase": 0,
                    "scenario": 1,
                    "stage": 1,
                    "block": 1,
                    "uid": 10,
                    "value": 100.0,
                },
            ]
        ),
        out / "flowp_sol.parquet",
    )
    # flown: block 2 → 50 MW reverse; block 1 → 0
    pq.write_table(
        pa.Table.from_pylist(
            [
                {
                    "scene": 0,
                    "phase": 0,
                    "scenario": 1,
                    "stage": 1,
                    "block": 2,
                    "uid": 10,
                    "value": 50.0,
                },
            ]
        ),
        out / "flown_sol.parquet",
    )
    tot = compute_gtopt_energy_totals(case_dir)
    # 0.04 × (100² × 2 + 50² × 3) = 1100 MWh
    assert tot["losses_line_analytic_mwh"] == pytest.approx(1100)
    # losses_mwh now aliases the analytic value (preferred over residual).
    assert tot["losses_mwh"] == pytest.approx(1100)


def test_compute_gtopt_energy_totals_extras_reported_as_diagnostic(
    tmp_path: Path,
) -> None:
    """When gtopt emits ``Line/loss_sol.parquet`` (opt-in via
    ``--write-out ...,extras``), the comparator reports it as a
    DIAGNOSTIC field (``losses_line_extras_mwh``) but does NOT use it
    as the headline ``losses_mwh``.

    Reason: gtopt's midpoint-debiased PWL produces a structurally
    smaller per-block loss than the analytic R/V²·f² quadratic that
    both gtopt's and PLEXOS's PWLs approximate.  For PLEXOS comparison
    the analytic is the apples-to-apples truth both sides target.
    The extras stream is useful only for diagnosing LP-internal
    behaviour (e.g. detecting under-charging that lets generation
    under-dispatch the physics).
    """
    import pyarrow as pa
    import pyarrow.parquet as pq

    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    bundle_path = case_dir / "bundle.json"
    bundle = json.loads(bundle_path.read_text())
    bundle["system"]["line_array"] = [
        {"uid": 10, "name": "L1", "resistance": 0.04, "voltage": 1.0},
    ]
    bundle_path.write_text(json.dumps(bundle))
    out = case_dir / "output" / "Line"
    out.mkdir(parents=True, exist_ok=True)
    # Flow parquets present (so the analytic path yields 1100 MWh).
    pq.write_table(
        pa.Table.from_pylist(
            [{"block": 1, "uid": 10, "value": 100.0}],
        ),
        out / "flowp_sol.parquet",
    )
    pq.write_table(
        pa.Table.from_pylist(
            [{"block": 2, "uid": 10, "value": 50.0}],
        ),
        out / "flown_sol.parquet",
    )
    # Consolidated extras stream: 8 MW × 2h + 5 MW × 3h = 31 MWh —
    # intentionally different from the analytic so the assertion
    # confirms the analytic still wins despite extras being present.
    pq.write_table(
        pa.Table.from_pylist(
            [
                {"block": 1, "uid": 10, "value": 8.0},
                {"block": 2, "uid": 10, "value": 5.0},
            ],
        ),
        out / "loss_sol.parquet",
    )
    tot = compute_gtopt_energy_totals(case_dir)
    assert tot["losses_line_extras_mwh"] == pytest.approx(31)
    assert tot["losses_line_analytic_mwh"] == pytest.approx(1100)
    # losses_mwh stays on the analytic — the apples-to-apples PLEXOS
    # metric — not on the LP's internal extras.
    assert tot["losses_mwh"] == pytest.approx(1100)


def test_compute_gtopt_energy_totals_extras_absent_falls_back(
    tmp_path: Path,
) -> None:
    """``Line/loss_sol.parquet`` absent (run without ``extras`` write-out)
    → extras = 0, analytic wins."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    case_dir = tmp_path / "case"
    _write_gtopt_case(case_dir)
    bundle_path = case_dir / "bundle.json"
    bundle = json.loads(bundle_path.read_text())
    bundle["system"]["line_array"] = [
        {"uid": 10, "name": "L1", "resistance": 0.04, "voltage": 1.0},
    ]
    bundle_path.write_text(json.dumps(bundle))
    out = case_dir / "output" / "Line"
    out.mkdir(parents=True, exist_ok=True)
    pq.write_table(
        pa.Table.from_pylist([{"block": 1, "uid": 10, "value": 100.0}]),
        out / "flowp_sol.parquet",
    )
    pq.write_table(
        pa.Table.from_pylist([{"block": 2, "uid": 10, "value": 50.0}]),
        out / "flown_sol.parquet",
    )
    tot = compute_gtopt_energy_totals(case_dir)
    assert tot["losses_line_extras_mwh"] == pytest.approx(0)
    assert tot["losses_line_analytic_mwh"] == pytest.approx(1100)
    assert tot["losses_mwh"] == pytest.approx(1100)


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


# ----------------------------------------------------------------
# T8: _read_plexos_table prefers the pre-extracted cache over live
# mdb-export shell-out
# ----------------------------------------------------------------


def _write_zstd_cache(case_dir: Path, table: str, payload: str) -> None:
    """Materialise ``case_dir/plexos_cache/<table>.csv.zst`` from text."""
    import shutil
    import subprocess

    cache_dir = case_dir / "plexos_cache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    raw = cache_dir / f"{table}.csv"
    raw.write_text(payload, encoding="utf-8")
    zst = cache_dir / f"{table}.csv.zst"
    # zstd CLI: -q quiet, -f overwrite, -o output, input last.
    subprocess.run(
        ["zstd", "-q", "-f", "-o", str(zst), str(raw)],
        check=True,
    )
    raw.unlink()
    # Sanity guard: the helper is gated by shutil.which("zstd") at
    # call-site, but if the binary disappeared between checks we'd
    # silently end up with a missing cache and the test would pass
    # for the wrong reason.
    assert zst.exists(), "zstd CLI did not produce the cache file"
    _ = shutil  # keep the import live for the sanity guard above


def test_read_plexos_table_cache_hit_preferred_over_mdb_export(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When ``case_dir/plexos_cache/<table>.csv.zst`` exists, the live
    ``mdb-export`` shell-out is bypassed entirely.

    Locks the cache fast-path so a refactor can't silently re-shell
    to mdb-export on every comparison invocation — which on the CEN
    PCP scale (~10× tables × 100s of MB each) turns a 5-second
    comparison into a 5-minute one.
    """
    import shutil

    if shutil.which("zstd") is None:
        pytest.skip("zstd CLI not available")

    case_dir = tmp_path / "case"
    case_dir.mkdir()
    payload = "interval_id,period_id\n1,1\n2,1\n3,2\n"
    _write_zstd_cache(case_dir, "t_key", payload)

    def _boom(*_args, **_kwargs):
        raise AssertionError("mdb-export must NOT be invoked when the cache is present")

    monkeypatch.setattr("subprocess.check_output", _boom)

    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.touch()
    from plexos2gtopt.compare_with_plexos import _read_plexos_table

    out = _read_plexos_table(fake_accdb, "t_key", case_dir=case_dir)
    assert out == payload


def test_read_plexos_table_cache_miss_falls_through_to_mdb_export(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """No ``plexos_cache`` directory ⇒ the live ``mdb-export`` shell-
    out IS invoked.  Mirror-image of the cache-hit test above."""
    case_dir = tmp_path / "case"
    case_dir.mkdir()
    sentinel_csv = "key_id,property_id\n42,99\n"
    calls: list[list[str]] = []

    def _capture(cmd, **_kwargs):
        calls.append(list(cmd))
        return sentinel_csv

    monkeypatch.setattr("subprocess.check_output", _capture)
    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.touch()
    from plexos2gtopt.compare_with_plexos import _read_plexos_table

    out = _read_plexos_table(fake_accdb, "t_key", case_dir=case_dir)
    assert out == sentinel_csv
    # Exactly one shell-out, targeting the requested table.
    assert len(calls) == 1
    assert calls[0][0] == "mdb-export"
    assert calls[0][-1] == "t_key"


# ----------------------------------------------------------------
# T9: _sum_consumer_demand_field_mwh filters synthetic <bat>_dem uids
# ----------------------------------------------------------------


def test_sum_consumer_demand_excludes_synthetic_battery_uids(
    tmp_path: Path,
) -> None:
    """gtopt's C++ ``expand_batteries`` appends synthetic ``<bat>_dem``
    Demand rows at LP-build time (uid > max input demand uid).  The
    field-sum helper MUST restrict to the bundle's declared
    ``demand_array`` uids — otherwise the synthetic demand's load
    (which equals the battery's finp) gets double-counted into
    consumer demand.

    Audit: without the filter, a single 1000-MW battery synthetic
    demand alone added ~7.6 GWh of phantom "consumer" demand on
    the CEN PCP daily week, masking the real demand-vs-PLEXOS gap.
    """
    import pyarrow as pa
    import pyarrow.parquet as pq

    from plexos2gtopt.compare_with_plexos import _sum_consumer_demand_field_mwh

    case_dir = tmp_path / "case"
    out_dir = case_dir / "output" / "Demand"
    out_dir.mkdir(parents=True)
    rows = [
        # uid=1: real demand 50 MW × 2 blocks
        {
            "scene": 0,
            "phase": 0,
            "scenario": 1,
            "stage": 1,
            "block": 1,
            "uid": 1,
            "value": 50.0,
        },
        {
            "scene": 0,
            "phase": 0,
            "scenario": 1,
            "stage": 1,
            "block": 2,
            "uid": 1,
            "value": 50.0,
        },
        # uid=2: real demand 50 MW × 2 blocks
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
        # uid=3: synthetic <bat>_dem at 1000 MW × 2 blocks — MUST be filtered out.
        {
            "scene": 0,
            "phase": 0,
            "scenario": 1,
            "stage": 1,
            "block": 1,
            "uid": 3,
            "value": 1000.0,
        },
        {
            "scene": 0,
            "phase": 0,
            "scenario": 1,
            "stage": 1,
            "block": 2,
            "uid": 3,
            "value": 1000.0,
        },
    ]
    pq.write_table(pa.Table.from_pylist(rows), out_dir / "load_sol.parquet")
    # Bundle declares uids 1 + 2 as real consumer demands; uid 3 is
    # the synthetic battery-fed entry gtopt's expand_batteries adds
    # at LP-build time.
    bundle = {
        "system": {
            "demand_array": [
                {"uid": 1, "name": "load_north"},
                {"uid": 2, "name": "load_south"},
            ],
        },
    }
    durations = {1: 1.0, 2: 1.0}
    total = _sum_consumer_demand_field_mwh(
        case_dir, bundle, durations, "Demand/load_sol.parquet"
    )
    # 50 MW × 1 h × 2 blocks × 2 uids = 200 MWh; the 1000-MW
    # synthetic uid contributes ZERO.
    assert total == pytest.approx(200.0)


# ----------------------------------------------------------------
# compute_plexos_battery_operation — AC-side PIDs (520/521), not
# DC-side (523/524).  Reading the DC-side over-reports throughput
# by the inverter loss (~7-10%) and creates a phantom
# "battery over-cycling" gap vs gtopt.
# ----------------------------------------------------------------


@pytest.fixture
def mock_mdb_export_battery(monkeypatch) -> None:
    """Synthetic .accdb with one Battery, BOTH PID flavours present.

    PID 520 (AC-side discharge) = 8 MW per period
    PID 521 (AC-side charge)    = 10 MW per period
    PID 523 (DC-side charge)    = 12 MW per period  -- MUST be ignored
    PID 524 (DC-side discharge) = 6  MW per period  -- MUST be ignored

    Two periods, each 1 h, so the expected per-battery totals are
    discharge_mwh = 16, charge_mwh = 20.  If the function ever
    reads the DC-side props instead, the totals would be 12 / 24.
    """
    tables: dict[str, list[dict[str, str]]] = {
        "t_phase_3": [
            {"period_id": "1", "interval_id": "1"},
            {"period_id": "2", "interval_id": "2"},
        ],
        "t_object": [
            {"object_id": "1", "class_id": "7", "name": "BAT_A", "category_id": "0"},
        ],
        "t_membership": [
            {"membership_id": "100", "child_object_id": "1"},
        ],
        "t_key": [
            {
                "key_id": "1",
                "membership_id": "100",
                "property_id": str(PLEXOS_PROP_BATTERY_GENERATION),
            },
            {
                "key_id": "2",
                "membership_id": "100",
                "property_id": str(PLEXOS_PROP_BATTERY_LOAD),
            },
            {
                "key_id": "3",
                "membership_id": "100",
                "property_id": str(PLEXOS_PROP_BATTERY_CHARGING),
            },
            {
                "key_id": "4",
                "membership_id": "100",
                "property_id": str(PLEXOS_PROP_BATTERY_DISCHARGING),
            },
        ],
        "t_data_0": [
            {"key_id": "1", "period_id": "1", "value": "8"},
            {"key_id": "1", "period_id": "2", "value": "8"},
            {"key_id": "2", "period_id": "1", "value": "10"},
            {"key_id": "2", "period_id": "2", "value": "10"},
            {"key_id": "3", "period_id": "1", "value": "12"},
            {"key_id": "3", "period_id": "2", "value": "12"},
            {"key_id": "4", "period_id": "1", "value": "6"},
            {"key_id": "4", "period_id": "2", "value": "6"},
        ],
    }

    def fake_check_output(cmd, **kwargs):
        assert cmd[0] == "mdb-export"
        return _mock_table(tables.get(cmd[2], []))

    def fake_run(cmd, **kwargs):
        if cmd and cmd[0] == "mdb-export":
            return mock.Mock(
                stdout=_mock_table(tables.get(cmd[2], [])),
                returncode=0,
            )
        raise NotImplementedError(f"unmocked subprocess: {cmd!r}")

    monkeypatch.setattr("subprocess.check_output", fake_check_output)
    monkeypatch.setattr("subprocess.run", fake_run)


def test_compute_plexos_battery_operation_uses_ac_side_pids(
    tmp_path: Path, mock_mdb_export_battery
) -> None:
    """LOCKED: per-battery totals use AC-side PIDs 520/521.

    Reading the DC-side 523/524 would have given
    discharge=12, charge=24 — both wrong; this test pins the
    correct values 16 and 20.  Reverts to a 523/524 reader will
    fail loudly here, preventing the phantom over-cycling gap from
    re-appearing in compare reports.
    """
    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.touch()
    out = compute_plexos_battery_operation(fake_accdb)
    assert set(out) == {"BAT_A"}
    bat = out["BAT_A"]
    # AC-side: 8 MW × 1 h × 2 periods = 16 MWh discharge.
    assert bat["discharge_mwh"] == pytest.approx(16.0)
    # AC-side: 10 MW × 1 h × 2 periods = 20 MWh charge.
    assert bat["charge_mwh"] == pytest.approx(20.0)
    # Net = discharge − charge.
    assert bat["net_mwh"] == pytest.approx(-4.0)
    # Sanity: had the function used DC-side props the values would
    # have been 12 and 24 — assert we're definitely NOT seeing those.
    assert bat["discharge_mwh"] != pytest.approx(12.0)
    assert bat["charge_mwh"] != pytest.approx(24.0)
