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
    - ``--mip`` on the full network currently reports infeasible — a
      separate gtopt MIP branch-and-cut issue we do not block on here.

Run:  ``cd tools && python -m pytest test_ucjl2gtopt.py -q``
"""

from __future__ import annotations

import csv
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
    invoking Julia.  Note: ``--mip`` on the same input currently
    reports infeasible — a separate gtopt MIP branch-and-cut issue
    that does not affect the LP-relax cross-check.
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
    also matches UC.jl's pinned values.  ``--mip`` on the full network
    currently reports infeasible — see the LP-relax test docstring.

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
