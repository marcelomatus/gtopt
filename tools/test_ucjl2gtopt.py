"""Tests for ``tools/ucjl2gtopt.py`` — UnitCommitment.jl → gtopt converter.

Three layers:

* **Unit / shape checks** — drive the converter on a tiny synthetic UC.jl
  JSON and assert that the produced gtopt system JSON has the expected
  topology counts, that the per-block hourly time series for Load
  round-trips into ``Demand.lmax``, and that the piecewise cost curve
  collapses to ``Commitment.pmax_segments + heat_rate_segments``.

* **Field-mapping check** against the UC.jl ``case14/base.json`` fixture
  (vendored from `ANL-CEEESA/UnitCommitment.jl`).  Verifies that the
  per-generator commitment metadata (tiered startup costs, min up/down
  time, ramp limits, initial status) is correctly translated into
  ``Commitment`` elements.

* **End-to-end** — only when a ``gtopt`` binary is reachable:

    - **LP-relax + full network** reproduces UC.jl's pinned commitment
      ``g1 = g2 = [1, 1, 1, 1]`` on the **full transmission network**
      (no copperplate flattening, no MIP solver required).  The
      reserve credit on the spinning-reserve constraint pushes both
      commitments to integer 1.0 spontaneously.  This is our headline
      cross-validation against UC.jl's HiGHS-solved golden.
    - **MIP + copperplate** reproduces the same pattern; pinning
      requires a MIP solver (CPLEX / Gurobi / MindOpt).
    - **MIP + full network** also solves cleanly after the
      ``add_col`` fix that pins ``col.scale = 1`` for integer columns
      (``test_real_case14_base_mip_full_network_status_clean_binary``
      is the regression anchor for that fix).

Run:  ``cd tools && python -m pytest test_ucjl2gtopt.py -q``
"""

from __future__ import annotations

import csv
import gzip
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


_TOOLS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TOOLS_DIR.parent
_CONVERTER = _TOOLS_DIR / "ucjl2gtopt.py"
_TEST_DATA_DIR = _TOOLS_DIR / "test_data"
_VENDORED_CASE14_BASE = _TEST_DATA_DIR / "UnitCommitmentJl_case14_base.json"
_VENDORED_CASE14_CONGESTED = _TEST_DATA_DIR / "UnitCommitmentJl_case14_congested.json"
_VENDORED_CASE14_FLEX = _TEST_DATA_DIR / "UnitCommitmentJl_case14_flex.json"
_VENDORED_BASE_WITH_STORAGE = _TEST_DATA_DIR / "UnitCommitmentJl_base_with_storage.json"
_VENDORED_CASE118_INITCOND = (
    _TEST_DATA_DIR / "UnitCommitmentJl_case118_initcond.json.gz"
)


# ---------------------------------------------------------------------------
# Synthetic UC.jl fixture — 2 buses, 4-hour horizon
# ---------------------------------------------------------------------------
#
# Hourly demand (matches a real UC.jl time series, not a flattened scalar):
#   b1 = [100, 110, 120, 100],  b2 = [50, 55, 60, 50]
#
# Generators (piecewise costs collapsed to first-segment slope):
#   g1 at b1 — pmin=50, pmax=200, gcost=30, capacity 200.
#   g2 at b2 — pmin=0,  pmax=100, gcost=20, capacity 100.
#
# Optimal LP-relaxation dispatch (cheapest order = g2, then g1 at pmin):
#   Hour 1: 100 + 50 → cost = 2000 + 1500 = 3500
#   Hour 2: 100 + 65 → cost = 2000 + 1950 = 3950
#   Hour 3: 100 + 80 → cost = 2000 + 2400 = 4400
#   Hour 4: 100 + 50 → cost = 2000 + 1500 = 3500
#   Total objective (scale_objective = 1.0)  → 15 350 $.

_SYNTHETIC_UCJL: dict = {
    "Parameters": {"Time horizon (h)": 4},
    "Buses": {
        "b1": {"Load (MW)": [100, 110, 120, 100]},
        "b2": {"Load (MW)": [50, 55, 60, 50]},
    },
    "Generators": {
        "g1": {
            "Bus": "b1",
            "Type": "Thermal",
            # Two-breakpoint curve (single slope) — avoids gtopt's
            # strictly-increasing-heat-rate convexity check on piecewise
            # segments while keeping the analytical 15 350 optimum below.
            "Production cost curve (MW)": [50, 200],
            "Production cost curve ($)": [1500, 6000],
        },
        "g2": {
            "Bus": "b2",
            "Type": "Thermal",
            "Production cost curve (MW)": [0, 100],
            "Production cost curve ($)": [0, 2000],
        },
    },
    "Transmission lines": {
        "L1": {
            "Source bus": "b1",
            "Target bus": "b2",
            "Reactance (ohms)": 0.1,
            "Normal flow limit (MW)": 100,
        }
    },
}

_SYNTHETIC_EXPECTED_OBJ = 15_350.0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _write_ucjl(tmp: Path, payload: dict | None = None) -> Path:
    path = tmp / "uc_smoke.json"
    path.write_text(json.dumps(payload if payload is not None else _SYNTHETIC_UCJL))
    return path


def _run_converter(
    ucjl_path: Path, out_path: Path, *extra_args: str
) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            sys.executable,
            str(_CONVERTER),
            str(ucjl_path),
            "-o",
            str(out_path),
            *extra_args,
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )


def _find_gtopt_binary() -> str | None:
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and Path(env_bin).is_file():
        return env_bin
    for rel in ("build/standalone/gtopt", "build/gtopt", "all/build/gtopt"):
        candidate = _REPO_ROOT / rel
        if candidate.is_file():
            return str(candidate)
    return shutil.which("gtopt")


def _has_mip_solver(gtopt_bin: str) -> bool:
    """``gtopt --solvers`` lists CPLEX / Gurobi when a MIP solver is loaded."""
    try:
        proc = subprocess.run(
            [gtopt_bin, "--solvers"],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    output = (proc.stdout + proc.stderr).lower()
    return any(s in output for s in ("cplex", "gurobi", "mindopt"))


def _read_solution_status(results_dir: Path) -> tuple[int | None, float | None]:
    csv_path = results_dir / "solution.csv"
    if not csv_path.exists():
        return None, None
    with csv_path.open(encoding="utf-8") as fh:
        rows = list(csv.reader(fh))
    if len(rows) < 2 or "status" not in rows[0]:
        return None, None
    header = rows[0]
    idx_status = header.index("status")
    idx_obj = header.index("obj_value") if "obj_value" in header else -1
    try:
        status = int(rows[1][idx_status])
    except (ValueError, IndexError):
        status = None
    try:
        obj = float(rows[1][idx_obj]) if idx_obj >= 0 else None
    except (ValueError, IndexError):
        obj = None
    return status, obj


def _read_commitment_status(results_dir: Path, gen_uid: int) -> list[float]:
    """Return the per-block status_sol values for the commitment whose uid matches.

    The output file lives at ``Commitment/status_sol_s0_p0.csv`` with one
    column per commitment uid (header ``"uid:1","uid:2",...``).
    """
    csv_path = results_dir / "Commitment" / "status_sol_s0_p0.csv"
    if not csv_path.exists():
        return []
    with csv_path.open(encoding="utf-8") as fh:
        rows = list(csv.reader(fh))
    if len(rows) < 2:
        return []
    header = rows[0]
    target = f"uid:{gen_uid}"
    if target not in header:
        return []
    idx = header.index(target)
    return [float(r[idx]) for r in rows[1:]]


def _solve(gtopt_bin: str, json_path: Path, tmp_run: Path) -> tuple[int, str]:
    tmp_run.mkdir(exist_ok=True)
    case_json = tmp_run / json_path.name
    case_json.write_bytes(json_path.read_bytes())
    proc = subprocess.run(
        [gtopt_bin, case_json.name],
        cwd=str(tmp_run),
        capture_output=True,
        text=True,
        check=False,
        timeout=180,
    )
    return proc.returncode, proc.stdout + proc.stderr


# ---------------------------------------------------------------------------
# Synthetic 2-bus regression
# ---------------------------------------------------------------------------


def test_converter_topology_counts(tmp_path: Path) -> None:
    """Buses / generators / demands / lines / commitments round-trip with expected counts."""
    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    data = json.loads(out.read_text())
    system = data["system"]
    assert len(system["bus_array"]) == 2
    assert len(system["generator_array"]) == 2
    assert len(system["demand_array"]) == 2
    assert len(system["line_array"]) == 1
    assert len(system["commitment_array"]) == 2  # one per thermal
    # The synthetic fixture has no ``Reserves`` block — reserve arrays
    # must be empty (no spurious ReserveZone / ReserveProvision entries).
    assert system["reserve_zone_array"] == []
    assert system["reserve_provision_array"] == []


def test_converter_emits_1x1xT_block_structure(tmp_path: Path) -> None:
    """1 scenario × 1 chronological stage × T blocks of duration 1 h."""
    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    sim = json.loads(out.read_text())["simulation"]
    assert len(sim["scenario_array"]) == 1
    assert sim["scenario_array"][0]["probability_factor"] == 1.0
    assert len(sim["stage_array"]) == 1
    stage = sim["stage_array"][0]
    assert stage["count_block"] == 4
    assert stage["chronological"] is True
    assert len(sim["block_array"]) == 4
    assert all(b["duration"] == 1 for b in sim["block_array"])


def test_converter_demand_lmax_preserves_hourly_time_series(tmp_path: Path) -> None:
    """``Demand.lmax = [[L_t for t in 0..T-1]]`` — the full hourly profile, not [0]."""
    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    demands = {
        d["name"]: d for d in json.loads(out.read_text())["system"]["demand_array"]
    }
    assert demands["d1"]["lmax"] == [[100.0, 110.0, 120.0, 100.0]]
    assert demands["d2"]["lmax"] == [[50.0, 55.0, 60.0, 50.0]]


def test_converter_emits_commitment_per_thermal(tmp_path: Path) -> None:
    """One ``Commitment`` per thermal generator, linked by gen name with ``relax=true``."""
    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    commits = {
        c["name"]: c for c in json.loads(out.read_text())["system"]["commitment_array"]
    }
    assert set(commits) == {"g1_uc", "g2_uc"}
    assert commits["g1_uc"]["generator"] == "g1"
    assert commits["g2_uc"]["generator"] == "g2"
    assert commits["g1_uc"]["relax"] is True
    assert commits["g2_uc"]["relax"] is True


def test_converter_handles_listoflist_cost_curve(tmp_path: Path) -> None:
    """v0.3 profiled gens wrap each piecewise breakpoint as a time series."""
    payload = json.loads(json.dumps(_SYNTHETIC_UCJL))
    payload["Generators"]["g1"]["Production cost curve (MW)"] = [
        [10, 11, 12, 10],
        [80, 82, 85, 80],
    ]
    payload["Generators"]["g1"]["Production cost curve ($)"] = [200, 2600]
    ucjl = _write_ucjl(tmp_path, payload)
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    gens = {
        g["name"]: g for g in json.loads(out.read_text())["system"]["generator_array"]
    }
    # Hour-0 collapse: pmin = 10, pmax = 80, slope = (2600-200)/(80-10) ≈ 34.286.
    assert gens["g1"]["pmin"] == 10.0
    assert gens["g1"]["pmax"] == 80.0
    assert gens["g1"]["gcost"] == pytest.approx(2400.0 / 70.0, rel=1e-6)


def test_converter_accepts_time_h_alias(tmp_path: Path) -> None:
    """v0.3 renamed ``Time horizon (h)`` to ``Time (h)``."""
    payload = json.loads(json.dumps(_SYNTHETIC_UCJL))
    payload["Parameters"] = {"Time (h)": 7}
    ucjl = _write_ucjl(tmp_path, payload)
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    sim = json.loads(out.read_text())["simulation"]
    assert sim["stage_array"][0]["count_block"] == 7
    assert len(sim["block_array"]) == 7


def test_converter_rejects_missing_input(tmp_path: Path) -> None:
    proc = _run_converter(tmp_path / "nope.json", tmp_path / "out.json")
    assert proc.returncode != 0


# ---------------------------------------------------------------------------
# End-to-end on the synthetic case
# ---------------------------------------------------------------------------


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
def test_synthetic_solves_with_gtopt(tmp_path: Path) -> None:
    """End-to-end: 2-bus synthetic → LP-relax solve → analytical optimum 15 350."""
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None

    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "g_synth.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj is not None
    assert obj == pytest.approx(_SYNTHETIC_EXPECTED_OBJ, rel=1e-6)


# ---------------------------------------------------------------------------
# UC.jl case14/base.json field-mapping
# ---------------------------------------------------------------------------


@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_topology_counts(tmp_path: Path) -> None:
    """UC.jl ``case14/base.json`` shape: 14 buses / 11 demands / 6 thermals / 20 lines / 6 commitments."""
    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out)
    assert proc.returncode == 0, proc.stderr

    system = json.loads(out.read_text())["system"]
    assert len(system["bus_array"]) == 14
    assert len(system["generator_array"]) == 6
    assert len(system["line_array"]) == 20
    assert len(system["commitment_array"]) == 6
    # 3 buses (b1 / b7 / b8) have zero load and are excluded.
    assert len(system["demand_array"]) == 11
    # One spinning zone (``r1``), five eligible thermals (g2..g6 — g1 has
    # no ``Reserve eligibility`` field).
    assert len(system["reserve_zone_array"]) == 1
    assert len(system["reserve_provision_array"]) == 5


@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_reserve_field_mapping(tmp_path: Path) -> None:
    """UC.jl ``Reserves`` and ``Reserve eligibility`` map onto gtopt elements.

    Zone ``r1``: ``Amount (MW) = 100`` → ``urreq = [[100, 100, 100, 100]]``
    (per-block schedule), ``Shortfall penalty ($/MW) = 1000`` → ``urcost``.
    Each eligible generator gets a ``ReserveProvision`` with
    ``ur_provision_factor = 1.0`` and ``urmax = gen.pmax`` — those two
    fields are both required by gtopt's LP build path
    (``ReserveProvisionLP::add_to_lp`` early-returns when either is
    missing — see source/reserve_provision_lp.cpp:49,101).
    """
    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out)
    assert proc.returncode == 0, proc.stderr

    system = json.loads(out.read_text())["system"]

    zones = {z["name"]: z for z in system["reserve_zone_array"]}
    assert "r1" in zones
    assert zones["r1"]["urreq"] == [[100.0, 100.0, 100.0, 100.0]]
    assert zones["r1"]["urcost"] == 1000.0

    provisions = {p["name"]: p for p in system["reserve_provision_array"]}
    # g1 has no ``Reserve eligibility`` in UC.jl → no provision row.
    assert "g1_rp" not in provisions
    # g2..g6 all carry ``Reserve eligibility = ["r1"]``.
    for gname, gpmax in (
        ("g2", 140.0),
        ("g3", 100.0),
        ("g4", 100.0),
        ("g5", 100.0),
        ("g6", 100.0),
    ):
        rp = provisions[f"{gname}_rp"]
        assert rp["generator"] == gname
        assert rp["reserve_zones"] == ["r1"]
        assert rp["ur_provision_factor"] == 1.0
        assert rp["urmax"] == [[gpmax] * 4]


@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_commitment_field_mapping(tmp_path: Path) -> None:
    """Per-gen ``Commitment`` matches UC.jl's source fields.

    ``g1`` has 3-tier startup costs and no min up/down or ramps.
    ``g2`` has 2-tier startup, ramp limits, and ``Minimum uptime = 4``.
    Pinning these catches any silent field-mapping drift in the converter.
    """
    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out)
    assert proc.returncode == 0, proc.stderr

    commits = {
        c["name"]: c for c in json.loads(out.read_text())["system"]["commitment_array"]
    }

    g1 = commits["g1_uc"]
    assert g1["generator"] == "g1"
    assert g1["initial_status"] == 0.0
    assert g1["initial_hours"] == 100.0
    assert g1["hot_start_cost"] == 1000.0
    assert g1["warm_start_cost"] == 1500.0
    assert g1["cold_start_cost"] == 2000.0
    assert g1["hot_start_time"] == 1.0
    assert g1["cold_start_time"] == 3.0
    # Piecewise heat-rate segmentation from the 4-breakpoint cost curve.
    assert g1["pmax_segments"] == [110.0, 130.0, 135.0]
    assert g1["heat_rate_segments"] == [20.0, 30.0, 40.0]

    g2 = commits["g2_uc"]
    assert g2["min_up_time"] == 4
    assert g2["min_down_time"] == 4
    assert g2["ramp_up"] == 98.0
    assert g2["ramp_down"] == 98.0
    assert g2["startup_ramp"] == 98.0
    assert g2["shutdown_ramp"] == 98.0
    assert g2["initial_status"] == 0.0
    assert g2["initial_hours"] == 8.0
    # 2-tier startup costs collapsed into hot/cold (no warm tier).
    assert g2["hot_start_cost"] == 3000.0
    assert g2["cold_start_cost"] == 4000.0


# ---------------------------------------------------------------------------
# UC.jl case14/base.json end-to-end
# ---------------------------------------------------------------------------


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_lprelax_solves(tmp_path: Path) -> None:
    """LP-relaxation on the full network: converter output is consumable by gtopt.

    Note: ``obj_value`` may be negative — gtopt's reserve formulation
    rewards provision (cost = ``-urcost`` on the requirement column at
    source/reserve_zone_lp.cpp:47-52), so an LP that satisfies the
    full 100 MW spinning-reserve requirement earns ``-100 × 1000 ×
    blocks`` of "reserve credit" that can outweigh dispatch + startup.
    We only assert the LP is feasible and the objective is finite.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out)
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj is not None and math.isfinite(obj)


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_lprelax_matches_ucjl_full_network(tmp_path: Path) -> None:
    """LP-relax + full network: ``g1`` and ``g2`` status = ``[1, 1, 1, 1]`` per UC.jl.

    ``test/src/usage_test.jl::usage_deterministic_test`` in
    ANL-CEEESA/UnitCommitment.jl pins, for the same vendored
    ``case14/base.json`` (with ``ShiftFactorsTransmissionExt`` for
    transmission)::

        sol["Thermal: Is on"]["g1"] == [1.0, 1.0, 1.0, 1.0]
        sol["Thermal: Is on"]["g2"] == [1.0, 1.0, 1.0, 1.0]

    With the converter mapping UC.jl's ``Reserves`` block onto
    ``ReserveZone`` + ``ReserveProvision`` — and with gtopt's
    ``Commitment.relax = true`` (LP relaxation) — gtopt reproduces
    **both** commitments on the **full transmission network**, no
    copperplate flattening required.  The reserve credit
    (``-urcost × provision_sum``) makes the optimal LP-relax
    solution commit g1 and g2 to integer 1.0 spontaneously.

    Cross-validates against UC.jl's HiGHS-solved golden without
    invoking Julia.

    ``--mip`` on the same input is also feasible (see
    ``test_real_case14_base_mip_full_network_status_clean_binary``
    below) since the integer-column-scaling bug in
    ``include/gtopt/linear_problem.hpp::add_col`` was fixed — integer
    columns now bypass the auto-scaling layer so the LP-side bound
    stays at a clean ``[0, 1]`` and CPLEX can hit physical ``u = 1.0``.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out)  # LP-relax default
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, _ = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0

    # g1 and g2 are commitment uids 1 and 2 (insertion order in the
    # commitment_array — gens are emitted in source dict order so
    # g1 → uid 1, g2 → uid 2).  UC.jl pins integer 1.0 — allow a tiny
    # LP numerical tolerance.
    for gname, gen_uid in (("g1", 1), ("g2", 2)):
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        assert len(status_per_block) == 4, (
            f"{gname}: expected 4 status values, got {status_per_block}"
        )
        assert all(v >= 0.99 for v in status_per_block), (
            f"{gname} commitment status {status_per_block} disagrees "
            f"with UC.jl's pinned [1, 1, 1, 1] golden"
        )


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_copperplate_mip_matches_ucjl(tmp_path: Path) -> None:
    """MIP + copperplate: ``g1`` and ``g2`` status = ``[1, 1, 1, 1]`` per UC.jl.

    Companion to the LP-relax test above.  Verifies that gtopt's MIP
    (with ``--copperplate`` so the transmission network is flattened)
    also matches UC.jl's pinned values.

    Skipped when no MIP solver plugin is loaded.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None
    if not _has_mip_solver(gtopt_bin):
        pytest.skip("no MIP solver plugin loaded (need CPLEX / Gurobi / MindOpt)")

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out, "--mip", "--copperplate")
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, _ = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0

    for gname, gen_uid in (("g1", 1), ("g2", 2)):
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        assert len(status_per_block) == 4, (
            f"{gname}: expected 4 status values, got {status_per_block}"
        )
        assert all(v >= 0.99 for v in status_per_block), (
            f"{gname} commitment status {status_per_block} disagrees "
            f"with UC.jl's pinned [1, 1, 1, 1] golden"
        )


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_mip_full_network_status_clean_binary(
    tmp_path: Path,
) -> None:
    """MIP + full network: integer-column-scaling bug fix anchor.

    Pre-fix, ``commitment_status`` columns came out with non-integer
    LP-side upper bounds (``11.6189`` for g1, ``11.8321`` for g2) because
    Ruiz / row-equilibration scaling rescaled the column based on KVL row
    coefficients.  CPLEX, treating the column as ``General`` integer, was
    then restricted to integer values in ``[0, 11]`` mapping to physical
    ``u ∈ {0, 0.086, …, 0.947}`` — physical ``u = 1`` was unreachable, so
    the MIP either silently capped commitment at 94.7 % or (with
    ``must_run``) reported infeasible.

    The fix in ``include/gtopt/linear_problem.hpp::add_col`` pins
    ``col.scale = 1.0`` for any column with ``is_integer = true``, so
    binary ``[0, 1]`` bounds round-trip through the LP build untouched
    and the MIP can reach ``u = 1`` for every generator.

    This test pins three invariants:

      1. The MIP solves (no infeasible exit).
      2. Every ``status_sol`` value is a clean integer (within 1e-6 of 0
         or 1) — the smoking-gun pre-fix value was 0.947 caps.
      3. The MIP objective matches the LP-relaxation objective for the
         same case (both equal -377 608.40).  A non-matching obj would
         indicate the MIP found a worse integer corner than the LP
         bound, which would re-open the regression.

    Skipped when no MIP solver plugin is loaded.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None
    if not _has_mip_solver(gtopt_bin):
        pytest.skip("no MIP solver plugin loaded (need CPLEX / Gurobi / MindOpt)")

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out, "--mip")
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0, f"MIP exit status = {status}, expected 0 (feasible/optimal)"

    # Invariant: every commitment_status value is integer (0 or 1).
    # Pre-fix the LP allowed CPLEX to pick fractional physical u like
    # 11/11.6189 ≈ 0.947 — those values would land in (0, 1) here.
    for gen_uid in range(1, 7):
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        for block_idx, value in enumerate(status_per_block):
            assert abs(value) <= 1e-6 or abs(value - 1.0) <= 1e-6, (
                f"g{gen_uid} block {block_idx + 1}: status_sol = {value} "
                f"is not a clean 0/1 integer — the integer-column-scaling "
                f"bug in apply_ruiz_scaling has regressed"
            )

    # Invariant: MIP optimum reaches the LP-relax bound (no integer gap).
    # Pre-fix the MIP couldn't reach physical u = 1 on g1/g2 so the optimal
    # MIP was a strictly worse integer corner with obj ≈ +4 079 092 (curtail
    # 40 MW × $100 K/MW).  Post-fix the MIP attains the LP-relax obj.
    assert obj == pytest.approx(-377_608.40, abs=1.0), (
        f"MIP obj = {obj}, expected ≈ -377 608.40 (matches LP-relax) — "
        f"a positive obj near +4 M would indicate the integer-scaling "
        f"regression where MIP can't commit g1/g2 and curtails instead"
    )

    # Invariant: g1 is committed in every block (matches UC.jl's pinned
    # golden in the deterministic test).  g2's per-block pattern is
    # degenerate at the optimum (multiple integer corners with the same
    # obj differ on which block g2 is on), so we don't pin it here — see
    # test_real_case14_base_lprelax_matches_ucjl_full_network for the
    # exact pattern on the LP-relax side.
    g1_status = _read_commitment_status(tmp_path / "run" / "output", gen_uid=1)
    assert g1_status == [1.0, 1.0, 1.0, 1.0], (
        f"g1 MIP status = {g1_status}, expected [1, 1, 1, 1] per "
        f"UC.jl's usage_deterministic_test pin"
    )


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_base_mip_full_network_row_max_equilibration(
    tmp_path: Path,
) -> None:
    """MIP + full network + ``row_max`` equilibration: same fix invariants.

    Companion to ``…_status_clean_binary`` above (which exercises the
    default Ruiz path for multi-bus KVL) — verifies the integer-column
    invariant holds equally under the ``row_max`` equilibration mode.
    ``row_max`` is the default for single-bus / no-KVL builds and scales
    rows only, so the integer columns naturally keep their physical
    bounds.  Pinning this here guards against a future change that
    layers column scaling on top of row_max (or swaps it for a
    Ruiz-style replacement).

    Skipped when no MIP solver plugin is loaded.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None
    if not _has_mip_solver(gtopt_bin):
        pytest.skip("no MIP solver plugin loaded (need CPLEX / Gurobi / MindOpt)")

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out, "--mip")
    assert proc.returncode == 0, proc.stderr

    # Force row_max equilibration via the JSON ``lp_matrix_options`` knob.
    data = json.loads(out.read_text())
    options = data.setdefault("options", {})
    options.setdefault("lp_matrix_options", {})["equilibration_method"] = "row_max"
    out.write_text(json.dumps(data))

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0, f"MIP exit status = {status}, expected 0"
    assert obj == pytest.approx(-377_608.40, abs=1.0)

    for gen_uid in range(1, 7):
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        for block_idx, value in enumerate(status_per_block):
            assert abs(value) <= 1e-6 or abs(value - 1.0) <= 1e-6, (
                f"g{gen_uid} block {block_idx + 1}: status_sol = {value} "
                f"is not a clean 0/1 integer under row_max equilibration"
            )

    g1_status = _read_commitment_status(tmp_path / "run" / "output", gen_uid=1)
    assert g1_status == [1.0, 1.0, 1.0, 1.0], (
        f"g1 row_max MIP status = {g1_status}, expected [1, 1, 1, 1]"
    )


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_BASE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_BASE}",
)
def test_real_case14_full_network_solves(tmp_path: Path) -> None:
    """Smoke check: full-network LP-relax produces a finite optimum.

    Companion to ``test_real_case14_base_lprelax_matches_ucjl_full_network``
    (which pins the commit pattern).  This test only guards the
    objective-value path — a regression that broke LP feasibility or
    the obj sign would fire here even if the commit-pattern assertions
    were softened.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_BASE, out)
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj is not None and math.isfinite(obj)


# ---------------------------------------------------------------------------
# Additional UC.jl literature fixtures
# ---------------------------------------------------------------------------
#
# Vendored from ``ANL-CEEESA/UnitCommitment.jl/test/fixtures/case14/``,
# same upstream provenance as ``case14/base.json`` but with parameter
# changes that exercise different commit dynamics on the IEEE-14 topology:
#
#   * ``congested.json`` — tighter line limits + g3's cost curve raised
#     to match the expensive thermals; the optimal MIP cannot avoid
#     ~250 MW of curtailment in every hour and produces a +29 M obj.
#   * ``flex.json``      — high-precision loads + ``flexiramp`` reserve
#     zones (which the converter intentionally drops since gtopt's
#     ``ReserveZone`` only models spinning reserves today).  Optimal
#     MIP commits g1+g2+g4+g5, leaves g3 off, +36 459 obj.
#
# Both pin gtopt's MIP optimum as the regression anchor.  This proves
# the converter shape + the integer-column-scaling fix hold across more
# than one literature case, not just ``case14/base.json``.


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_CONGESTED.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_CONGESTED}",
)
def test_real_case14_congested_mip_full_network(tmp_path: Path) -> None:
    """UC.jl ``case14/congested.json``: tighter lines force curtailment.

    Pins:

      * Solver status 0 (feasible / optimal).
      * Every ``status_sol`` value is a clean 0/1 integer (guards the
        column-scaling fix on a different load / cost profile from
        ``case14/base``).
      * gtopt MIP obj = ``+29 126 081.23`` — the high positive value
        reflects ``demand_fail`` curtailment that the tightened line
        limits make unavoidable in single-scenario mode.  UC.jl's
        stochastic test pairs this case with ``base.json`` and pins
        per-scenario production patterns; we don't model the stochastic
        coupling, so the single-scenario optimum is independent.

    Skipped when no MIP solver plugin is loaded.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None
    if not _has_mip_solver(gtopt_bin):
        pytest.skip("no MIP solver plugin loaded (need CPLEX / Gurobi / MindOpt)")

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_CONGESTED, out, "--mip")
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0, f"MIP exit status = {status}"
    assert obj == pytest.approx(29_126_081.23, rel=1e-5)

    # Clean-binary invariant (column-scaling-fix regression anchor).
    for gen_uid in range(1, 7):
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        for block_idx, value in enumerate(status_per_block):
            assert abs(value) <= 1e-6 or abs(value - 1.0) <= 1e-6, (
                f"g{gen_uid} block {block_idx + 1}: status_sol = {value} "
                f"is not a clean 0/1 integer on case14/congested"
            )


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14_FLEX.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14_FLEX}",
)
def test_real_case14_flex_mip_full_network(tmp_path: Path) -> None:
    """UC.jl ``case14/flex.json``: flexiramp reserves dropped, distinct optimum.

    The fixture's two ``Reserves`` zones are both ``Type = flexiramp``,
    which the converter intentionally drops (gtopt's ``ReserveZone``
    models spinning reserves only).  The result is a substantially
    different commit pattern from ``case14/base`` — g3 stays off, g1
    is committed every hour, and the MIP optimum lands at a small
    positive obj.

    Pins:

      * Solver status 0.
      * Every ``status_sol`` is clean 0/1.
      * gtopt MIP obj = ``+36 459.08``.
      * g1 status = ``[1, 1, 1, 1]`` (consistent across base / flex —
        g1 is always the cheapest base-load commit on this topology).

    Skipped when no MIP solver plugin is loaded.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None
    if not _has_mip_solver(gtopt_bin):
        pytest.skip("no MIP solver plugin loaded (need CPLEX / Gurobi / MindOpt)")

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_CASE14_FLEX, out, "--mip")
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj == pytest.approx(36_459.08, rel=1e-5)

    for gen_uid in range(1, 7):
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        for block_idx, value in enumerate(status_per_block):
            assert abs(value) <= 1e-6 or abs(value - 1.0) <= 1e-6, (
                f"g{gen_uid} block {block_idx + 1}: status_sol = {value} "
                f"is not a clean 0/1 integer on case14/flex"
            )

    g1_status = _read_commitment_status(tmp_path / "run" / "output", gen_uid=1)
    assert g1_status == [1.0, 1.0, 1.0, 1.0], (
        f"g1 flex MIP status = {g1_status}, expected [1, 1, 1, 1]"
    )


# ---------------------------------------------------------------------------
# UC.jl base.json (top-level) — extends coverage to Storage units / BESS
# ---------------------------------------------------------------------------
#
# Vendored from ``ANL-CEEESA/UnitCommitment.jl/test/fixtures/base.json``
# (distinct from ``case14/base.json``).  Larger system: 15 buses, 12
# thermal gens, 22 lines, 3 spinning-reserve zones, **2 Storage units**
# (BESS — su1 at b4, su2 at b5).  This is our regression anchor that
# the converter's Storage units → ``Battery`` mapping survives end-to-
# end (LP build via System::expand_batteries() auto-generates the
# discharge Generator + charge Demand + linking Converter) and that
# the resulting LP solves with sensible obj.


@pytest.mark.skipif(
    not _VENDORED_BASE_WITH_STORAGE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_BASE_WITH_STORAGE}",
)
def test_real_base_with_storage_topology_counts(tmp_path: Path) -> None:
    """UC.jl top-level ``base.json`` storage-aware shape check.

    Pins the converter output:

      * 15 buses, 12 thermal generators, 20 lines that survive the
        unknown-bus filter (out of 22 in the UC.jl source — 2 lines
        reference buses we don't emit because they have no load).
      * 10 ``Commitment`` entries (only thermal gens; the 2 profiled
        gens skip Commitment).
      * 3 ``ReserveZone`` + 7 ``ReserveProvision`` entries.
      * 2 ``Battery`` entries mapping UC.jl's ``Storage units``
        ``su1`` / ``su2``.  Spot-check the field mapping on ``su1``
        (scalar emax/emin) and ``su2`` (per-block emax list).
    """
    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_BASE_WITH_STORAGE, out)
    assert proc.returncode == 0, proc.stderr

    system = json.loads(out.read_text())["system"]
    assert len(system["bus_array"]) == 15
    assert len(system["generator_array"]) == 12
    # Topology: 22 lines in source; some reference unknown buses (filtered).
    assert len(system["line_array"]) == 20
    assert len(system["commitment_array"]) == 10
    assert len(system["reserve_zone_array"]) == 3
    assert len(system["battery_array"]) == 2

    bats = {b["name"]: b for b in system["battery_array"]}

    # su1: scalar Maximum/Minimum level — round-trips as scalar.
    assert bats["su1"]["emax"] == 100.0
    assert bats["su1"]["emin"] == 10.0
    assert bats["su1"]["pmax_charge"] == 50.0
    assert bats["su1"]["pmax_discharge"] == 40.0
    assert bats["su1"]["input_efficiency"] == 0.9
    assert bats["su1"]["output_efficiency"] == 0.95
    assert bats["su1"]["eini"] == 50.0
    assert bats["su1"]["efin"] == 20.0
    assert bats["su1"]["gcost"] == 1.5

    # su2: per-block list — round-trips as list (gtopt's
    # OptTRealFieldSched accepts scalar or list).
    assert bats["su2"]["emax"] == [200.0, 200.0, 200.0, 200.0]
    assert bats["su2"]["emin"] == [20.0, 20.0, 20.0, 20.0]
    assert bats["su2"]["pmax_charge"] == 80.0


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_BASE_WITH_STORAGE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_BASE_WITH_STORAGE}",
)
def test_real_base_with_storage_solves_lprelax(tmp_path: Path) -> None:
    """LP-relax with 2 BESS units: solver completes and Battery output exists.

    Catches regressions in the Storage → Battery mapping at the LP build
    level — if gtopt's ``expand_batteries()`` fails to instantiate the
    auto-generated Generator / Demand / Converter trio, the solve
    surfaces here.  Also asserts the ``Battery/`` output directory was
    populated (proves the LP built and solved with the battery cols /
    rows).
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_BASE_WITH_STORAGE, out)
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj is not None and math.isfinite(obj)

    # Battery output dir confirms the LP build instantiated the BESS
    # variables (efin / efmax / charge / discharge cols + balance rows).
    battery_dir = tmp_path / "run" / "output" / "Battery"
    assert battery_dir.is_dir(), (
        "Battery/ output dir missing — Storage → Battery mapping didn't "
        "create LP variables for the BESS units"
    )


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_BASE_WITH_STORAGE.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_BASE_WITH_STORAGE}",
)
def test_real_base_with_storage_mip_clean_binary(tmp_path: Path) -> None:
    """MIP with 2 BESS units: integer-column-scaling fix still holds.

    Larger LP than ``case14/base`` (15 buses, 12 gens, 22 lines + 2
    batteries that expand into auto-generated Generator / Demand /
    Converter sub-elements) — more rows and more column-scale
    candidates for the Ruiz equilibrator to play with.  This case
    independently verifies the integer-column scaling fix
    (``status_sol`` must stay clean 0/1) on a system where the
    auto-expanded battery sub-elements add their own coefficients to
    the bus-balance rows that ``Commitment`` columns also feed.

    Pins gtopt MIP obj = ``-376 357.36``.

    Skipped when no MIP solver plugin is loaded.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None
    if not _has_mip_solver(gtopt_bin):
        pytest.skip("no MIP solver plugin loaded (need CPLEX / Gurobi / MindOpt)")

    out = tmp_path / "g.json"
    proc = _run_converter(_VENDORED_BASE_WITH_STORAGE, out, "--mip")
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj == pytest.approx(-376_357.36, abs=1.0)

    # Integer-column scaling-fix invariant on every commitment.
    for gen_uid in range(1, 11):  # 10 commitments on the thermal gens
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        for block_idx, value in enumerate(status_per_block):
            assert abs(value) <= 1e-6 or abs(value - 1.0) <= 1e-6, (
                f"commitment uid={gen_uid} block {block_idx + 1}: "
                f"status_sol = {value} is not a clean 0/1 integer "
                f"on base_with_storage"
            )


# ---------------------------------------------------------------------------
# IEEE-118 UC benchmark (case118-initcond)
# ---------------------------------------------------------------------------
#
# Vendored from ``ANL-CEEESA/UnitCommitment.jl/test/fixtures/case118-initcond.json.gz``
# (24 KB gzipped, 200 KB uncompressed).  Standard IEEE-118 topology
# extended by UC.jl with realistic data-driven UC parameters:
#
#   * 118 buses (99 with non-zero load after the unknown-bus filter).
#   * 54 thermal generators — every one has the full UC field set
#     (5-breakpoint piecewise costs, 3-tier startup costs/delays,
#     ramps, min up/down, reserve eligibility).
#   * 186 transmission lines.
#   * 1 spinning-reserve zone with a per-hour time-varying
#     requirement (length-36 list — exercises the
#     ``Amount (MW)`` time-series path).
#   * 36-hour horizon.
#
# Resulting LP shape (LP-relax): ~25 800 vars × ~28 700 rows.  MIP
# (with the integer-column-scaling fix in place) closes the integer
# gap to ~0.86 % (LP obj 2 082 551, MIP obj 2 100 504) in well under
# the 180-s timeout.
#
# This is the largest UC.jl fixture in the test suite — significantly
# bigger than the case14 / 15-bus fixtures and a meaningful workload
# for the column-scaling fix at scale (CPLEX MIP has to assign
# integer values to 54 × 36 = 1944 commitment status variables plus
# their startup tiers).


@pytest.mark.skipif(
    not _VENDORED_CASE118_INITCOND.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE118_INITCOND}",
)
def test_real_case118_initcond_topology_counts(tmp_path: Path) -> None:
    """UC.jl ``case118-initcond.json.gz`` shape — 118-bus IEEE benchmark.

    Pins the converter output counts so a future converter regression
    that silently drops generators, lines, or reserve provisions
    surfaces here.  Also exercises:

      * Gzipped fixture round-trip (vendored as ``.json.gz`` to save
        repo space — ~24 KB vs ~200 KB uncompressed).
      * v0.3 ``Type = "spinning"`` (lowercase) handled identically
        to ``"Spinning"``.
      * Reserve ``Amount (MW)`` as a 36-element time series rather
        than a scalar.
    """
    ucjl = tmp_path / "case118.json"
    ucjl.write_bytes(gzip.decompress(_VENDORED_CASE118_INITCOND.read_bytes()))
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    system = json.loads(out.read_text())["system"]
    assert len(system["bus_array"]) == 118
    assert len(system["generator_array"]) == 54
    assert len(system["line_array"]) == 186
    assert len(system["commitment_array"]) == 54  # one per thermal
    assert len(system["reserve_zone_array"]) == 1
    assert len(system["reserve_provision_array"]) == 54  # every gen is eligible
    # 99 buses have non-zero load after the unknown-bus filter.
    assert len(system["demand_array"]) == 99

    # Reserve requirement is a 36-hour time series, not a scalar.
    zone = system["reserve_zone_array"][0]
    assert isinstance(zone["urreq"], list)
    assert len(zone["urreq"]) == 1  # one stage
    assert len(zone["urreq"][0]) == 36  # T = 36 hours
    # First hour of the UC.jl source = 61.19 MW.
    assert zone["urreq"][0][0] == pytest.approx(61.19, abs=1e-6)


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE118_INITCOND.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE118_INITCOND}",
)
def test_real_case118_initcond_lprelax_solves(tmp_path: Path) -> None:
    """LP-relax solve on the 118-bus benchmark (~26 K cols × ~29 K rows).

    Pins ``obj = 2 082 550.73``.  A regression that drops generators
    or mis-builds the piecewise cost matrix would shift this obj
    materially; a regression in the reserve time-series path would
    drop the reserve-credit term and push obj higher.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None

    ucjl = tmp_path / "case118.json"
    ucjl.write_bytes(gzip.decompress(_VENDORED_CASE118_INITCOND.read_bytes()))
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj == pytest.approx(2_082_550.73, rel=1e-5)


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE118_INITCOND.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE118_INITCOND}",
)
def test_real_case118_initcond_mip_clean_binary(tmp_path: Path) -> None:
    """MIP on the 118-bus benchmark: integer-scaling fix at scale.

    Pins three invariants on the largest UC.jl fixture in the suite:

      * Solver status 0.
      * gtopt MIP obj = ``2 100 504.38`` (~0.86 % integer gap over LP).
      * Every one of the **1944 commitment status values** (54 gens
        × 36 hours) is a clean 0/1 integer — the column-scaling-fix
        invariant on a workload large enough that any residual
        non-unit col_scale on the integer columns would corrupt the
        MIP across many variables, not just g1/g2.

    Skipped when no MIP solver plugin is loaded.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None
    if not _has_mip_solver(gtopt_bin):
        pytest.skip("no MIP solver plugin loaded (need CPLEX / Gurobi / MindOpt)")

    ucjl = tmp_path / "case118.json"
    ucjl.write_bytes(gzip.decompress(_VENDORED_CASE118_INITCOND.read_bytes()))
    out = tmp_path / "g.json"
    proc = _run_converter(ucjl, out, "--mip")
    assert proc.returncode == 0, proc.stderr

    rc, log = _solve(gtopt_bin, out, tmp_path / "run")
    assert rc == 0, log

    status, obj = _read_solution_status(tmp_path / "run" / "output")
    assert status == 0
    assert obj == pytest.approx(2_100_504.38, abs=10.0)

    # Clean-binary invariant across all 1944 (54 gens × 36 hours) values.
    for gen_uid in range(1, 55):
        status_per_block = _read_commitment_status(
            tmp_path / "run" / "output", gen_uid=gen_uid
        )
        assert len(status_per_block) == 36, (
            f"uid={gen_uid}: expected 36 status values, got {len(status_per_block)}"
        )
        for block_idx, value in enumerate(status_per_block):
            assert abs(value) <= 1e-6 or abs(value - 1.0) <= 1e-6, (
                f"commitment uid={gen_uid} block {block_idx + 1}: "
                f"status_sol = {value} is not a clean 0/1 integer "
                f"on case118-initcond"
            )
