"""Tests for ``tools/ucjl2gtopt.py`` — UnitCommitment.jl → gtopt converter.

Two layers:

* **Unit / shape checks** — run on every pytest invocation.  Drive the
  converter on a tiny synthetic UC.jl JSON and assert that the produced
  gtopt system JSON has the expected counts, that the deliberate
  single-period flattening of UC.jl time series happens (Load,
  Normal flow limit), and that the average-slope cost reduction of the
  piecewise curve is what we documented.

* **Integration check** — only when a ``gtopt`` binary is available
  (``GTOPT_BIN``, ``$PATH``, or the in-tree ``build/standalone/gtopt``).
  Convert → solve → assert the LP is feasible (status=0) and the
  objective matches the analytical optimum.  Skipped otherwise so the
  unit checks remain usable in a Python-only environment.

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
_VENDORED_CASE14 = _TEST_DATA_DIR / "matpower_case14_2017-01-01.json.gz"


# ---------------------------------------------------------------------------
# Synthetic UC.jl fixture
# ---------------------------------------------------------------------------
#
# Two-bus, two-generator, one-line case with a 4-hour horizon.  The Load
# time series is intentionally non-flat so we can prove the converter
# flattens to ``[0]`` (a known limitation of the current single-period
# mapping — see the module docstring of ``tools/ucjl2gtopt.py``).
#
# Optimal dispatch (per hour, ignoring time index):
#   * g2 at b2 — gcost=20 $/MWh, capacity 100 → dispatch 100 MW.
#   * g1 at b1 — gcost=30 $/MWh, pmin=50      → dispatch 50 MW (at pmin).
#   * Line b1↔b2 — carries the 50-MW surplus from b2 to b1.
# Hourly cost:    100 * 20 + 50 * 30 = 3500 $/h.
# Block duration: 4 h → total obj = 14 000 $.  Solver should report
# exactly this value (no demand shortfall, no scaling surprise — the
# converter sets ``scale_objective = 1000`` but gtopt's ``obj_value`` in
# ``solution.csv`` is reported in physical units).

_SYNTHETIC_UCJL: dict = {
    "Parameters": {"Time horizon (h)": 4},
    "Buses": {
        "b1": {"Load (MW)": [100, 110, 120, 100]},
        "b2": {"Load (MW)": [50, 55, 60, 50]},
    },
    "Generators": {
        "g1": {
            "Bus": "b1",
            "Production cost curve (MW)": [50, 100, 200],
            "Production cost curve ($)": [1500, 3000, 6000],
        },
        "g2": {
            "Bus": "b2",
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

_EXPECTED_OBJ_VALUE = 14_000.0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _write_ucjl(tmp: Path, payload: dict | None = None) -> Path:
    """Materialise a synthetic UC.jl JSON file under ``tmp``."""
    path = tmp / "uc_smoke.json"
    path.write_text(json.dumps(payload if payload is not None else _SYNTHETIC_UCJL))
    return path


def _run_converter(ucjl_path: Path, out_path: Path) -> subprocess.CompletedProcess:
    """Invoke ``tools/ucjl2gtopt.py`` as a CLI on ``ucjl_path``."""
    return subprocess.run(
        [sys.executable, str(_CONVERTER), str(ucjl_path), "-o", str(out_path)],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )


def _find_gtopt_binary() -> str | None:
    """Locate the gtopt binary for the end-to-end leg; ``None`` if absent."""
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and Path(env_bin).is_file():
        return env_bin
    for rel in ("build/standalone/gtopt", "build/gtopt", "all/build/gtopt"):
        candidate = _REPO_ROOT / rel
        if candidate.is_file():
            return str(candidate)
    return shutil.which("gtopt")


def _read_solution_status(results_dir: Path) -> tuple[int | None, float | None]:
    """Parse ``solution.csv`` → ``(status, obj_value)``."""
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


# ---------------------------------------------------------------------------
# Unit-level: converter shape + documented flattening behaviour
# ---------------------------------------------------------------------------


def test_converter_topology_counts(tmp_path: Path) -> None:
    """Buses / generators / demands / lines all round-trip with expected counts."""
    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "uc_smoke_gtopt.json"

    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr
    assert out.is_file()

    data = json.loads(out.read_text())
    system = data["system"]
    assert len(system["bus_array"]) == 2
    assert len(system["generator_array"]) == 2
    assert len(system["demand_array"]) == 2
    assert len(system["line_array"]) == 1

    # Single stage / block / scenario as documented.
    sim = data["simulation"]
    assert len(sim["stage_array"]) == 1
    assert len(sim["block_array"]) == 1
    assert sim["block_array"][0]["duration"] == 4  # T = "Time horizon (h)"
    assert len(sim["scenario_array"]) == 1


def test_converter_flattens_load_to_first_timestep(tmp_path: Path) -> None:
    """Documents the single-period limitation: only ``Load[0]`` is kept.

    UC.jl is hourly, gtopt v0 of this converter is single-block.  The
    flattening is intentional but lossy — if a future revision turns it
    into a proper hourly mapping this test will fire and force a
    documentation / golden refresh.
    """
    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "uc_smoke_gtopt.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    demands = {
        d["name"]: d for d in json.loads(out.read_text())["system"]["demand_array"]
    }
    # Load (MW) was [100, 110, 120, 100] / [50, 55, 60, 50] — only [0] survives.
    assert demands["d1"]["lmax"] == [[100.0]]
    assert demands["d2"]["lmax"] == [[50.0]]


def test_converter_uses_average_slope_for_piecewise_cost(tmp_path: Path) -> None:
    """Piecewise cost curve is collapsed to one slope = total / total.

    g1 curve (MW=[50,100,200], $=[1500,3000,6000]) → slope = 4500/150 = 30.
    g2 curve (MW=[0,100],      $=[0,2000])          → slope = 2000/100 = 20.
    pmin / pmax come from the first / last MW breakpoint.
    """
    ucjl = _write_ucjl(tmp_path)
    out = tmp_path / "uc_smoke_gtopt.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    gens = {
        g["name"]: g for g in json.loads(out.read_text())["system"]["generator_array"]
    }
    assert gens["g1"]["pmin"] == 50.0
    assert gens["g1"]["pmax"] == 200.0
    assert gens["g1"]["gcost"] == pytest.approx(30.0)
    assert gens["g2"]["pmin"] == 0.0
    assert gens["g2"]["pmax"] == 100.0
    assert gens["g2"]["gcost"] == pytest.approx(20.0)


def test_converter_skips_unknown_buses_in_lines(tmp_path: Path) -> None:
    """Lines whose endpoints reference an unknown bus are silently dropped.

    Guards against the converter crashing on partial UC.jl exports
    where the topology and the line list disagree.
    """
    payload = json.loads(json.dumps(_SYNTHETIC_UCJL))  # deep copy
    payload["Transmission lines"]["L_orphan"] = {
        "Source bus": "b1",
        "Target bus": "b_does_not_exist",
        "Reactance (ohms)": 0.05,
        "Normal flow limit (MW)": 50,
    }
    ucjl = _write_ucjl(tmp_path, payload)
    out = tmp_path / "uc_smoke_gtopt.json"
    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    lines = json.loads(out.read_text())["system"]["line_array"]
    assert [line["name"] for line in lines] == ["L1"]


def test_converter_rejects_missing_input(tmp_path: Path) -> None:
    """A non-existent input path must surface a non-zero exit code."""
    proc = _run_converter(tmp_path / "no_such_file.json", tmp_path / "out.json")
    assert proc.returncode != 0


# ---------------------------------------------------------------------------
# UC.jl v0.3 schema variants
# ---------------------------------------------------------------------------


def test_converter_accepts_time_h_alias(tmp_path: Path) -> None:
    """v0.3 renamed ``Time horizon (h)`` to ``Time (h)``.

    The converter must accept both — silently falling back to the
    default 24 was the pre-fix behavior and would have masked a wrong
    horizon for every v0.3 instance whose horizon isn't 24.
    """
    payload = json.loads(json.dumps(_SYNTHETIC_UCJL))
    payload["Parameters"] = {"Time (h)": 7}  # drop legacy key, use v0.3 alias
    ucjl = _write_ucjl(tmp_path, payload)
    out = tmp_path / "uc_smoke_gtopt.json"

    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    sim = json.loads(out.read_text())["simulation"]
    assert sim["block_array"][0]["duration"] == 7


def test_converter_handles_listoflist_cost_curve(tmp_path: Path) -> None:
    """v0.3 profiled gens wrap each piecewise breakpoint as a time series.

    Shape: ``Production cost curve (MW) = [[ts_low], [ts_high]]`` and
    ``Production cost curve ($) = [c_low, c_high]``.  The converter
    must collapse the inner time series to hour 0 (matching how Load
    and Normal flow limit are flattened) so the pmin/pmax/gcost
    extraction stays consistent.  Before the fix this raised
    ``TypeError: unsupported operand type(s) for -: 'list' and 'list'``
    on ``pglib-uc/rts_gmlc``.
    """
    payload = json.loads(json.dumps(_SYNTHETIC_UCJL))
    # Replace g1's flat scalar curve with a 2-segment time-varying curve.
    payload["Generators"]["g1"]["Production cost curve (MW)"] = [
        [10, 11, 12, 10],
        [80, 82, 85, 80],
    ]
    payload["Generators"]["g1"]["Production cost curve ($)"] = [200, 2600]
    ucjl = _write_ucjl(tmp_path, payload)
    out = tmp_path / "uc_smoke_gtopt.json"

    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    gens = {
        g["name"]: g for g in json.loads(out.read_text())["system"]["generator_array"]
    }
    # Hour-0 flatten: pmin=10, pmax=80, slope=(2600-200)/(80-10)=34.2857…
    assert gens["g1"]["pmin"] == 10.0
    assert gens["g1"]["pmax"] == 80.0
    assert gens["g1"]["gcost"] == pytest.approx(2400.0 / 70.0, rel=1e-6)


# ---------------------------------------------------------------------------
# Real-benchmark anchor: vendored MATPOWER case14 from axavier.org
# ---------------------------------------------------------------------------


@pytest.mark.skipif(
    not _VENDORED_CASE14.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14}",
)
def test_converter_handles_real_matpower_case14(tmp_path: Path) -> None:
    """Converter survives the real UC.jl ``matpower/case14/2017-01-01`` schema.

    Pure shape check — no gtopt involved — so this leg runs even when
    the binary isn't available.  Pins counts (14 buses / 5 gens / 20
    lines / 36-h horizon) so any future converter change that silently
    drops elements lights up here.
    """
    ucjl = tmp_path / "uc_case14.json"
    ucjl.write_bytes(gzip.decompress(_VENDORED_CASE14.read_bytes()))
    out = tmp_path / "g_case14.json"

    proc = _run_converter(ucjl, out)
    assert proc.returncode == 0, proc.stderr

    data = json.loads(out.read_text())
    system = data["system"]
    assert len(system["bus_array"]) == 14
    assert len(system["generator_array"]) == 5
    assert len(system["line_array"]) == 20
    # 11 buses have non-zero hour-0 load in this case (b1 / b7 / b8 are dry).
    assert len(system["demand_array"]) == 11
    assert data["simulation"]["block_array"][0]["duration"] == 36


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
@pytest.mark.skipif(
    not _VENDORED_CASE14.is_file(),
    reason=f"vendored UC.jl fixture missing: {_VENDORED_CASE14}",
)
def test_real_case14_solves_with_gtopt(tmp_path: Path) -> None:
    """Real-benchmark anchor: case14 → gtopt → obj_value = 350 275.71.

    Complements the analytical 2-bus synthetic case with a real UC.jl
    instance (14 buses, 5 thermal gens, 20 lines, 36-h horizon, real
    piecewise cost curves and time-series loads collapsed to hour 0).
    The golden was computed once by solving the converted JSON with
    the in-tree gtopt binary; any drift > 1e-4 (relative) indicates
    either a converter regression or a gtopt LP-build change.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None

    ucjl = tmp_path / "uc_case14.json"
    ucjl.write_bytes(gzip.decompress(_VENDORED_CASE14.read_bytes()))

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    case_json = run_dir / "g_case14.json"
    proc = _run_converter(ucjl, case_json)
    assert proc.returncode == 0, proc.stderr

    solve = subprocess.run(
        [gtopt_bin, case_json.name],
        cwd=str(run_dir),
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    assert solve.returncode == 0, (
        f"gtopt failed (rc={solve.returncode})\n"
        f"--- stdout ---\n{solve.stdout}\n"
        f"--- stderr ---\n{solve.stderr}\n"
    )

    status, obj = _read_solution_status(run_dir / "output")
    assert status == 0, f"solver status={status}, expected 0"
    assert obj is not None and not math.isnan(obj)
    assert obj == pytest.approx(350_275.71373979026, rel=1e-4)


# ---------------------------------------------------------------------------
# Integration: convert → gtopt solve → check solution.csv
# ---------------------------------------------------------------------------


@pytest.mark.skipif(_find_gtopt_binary() is None, reason="gtopt binary not found")
def test_converted_case_solves_with_gtopt(tmp_path: Path) -> None:
    """End-to-end: UC.jl JSON → gtopt JSON → solve → status=0, obj=14000.

    Anchors the converter's output schema against the live gtopt LP
    loader: any future change that breaks the JSON contract (renamed
    fields, missing arrays, type mismatches) lights up here even if
    the unit tests above still pass.
    """
    gtopt_bin = _find_gtopt_binary()
    assert gtopt_bin is not None  # guarded by skipif

    ucjl = _write_ucjl(tmp_path)
    out_json = tmp_path / "uc_smoke_gtopt.json"
    proc = _run_converter(ucjl, out_json)
    assert proc.returncode == 0, proc.stderr

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    case_json = run_dir / "uc_smoke_gtopt.json"
    case_json.write_bytes(out_json.read_bytes())

    solve = subprocess.run(
        [gtopt_bin, case_json.name],
        cwd=str(run_dir),
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    assert solve.returncode == 0, (
        f"gtopt failed (rc={solve.returncode})\n"
        f"--- stdout ---\n{solve.stdout}\n"
        f"--- stderr ---\n{solve.stderr}\n"
    )

    status, obj = _read_solution_status(run_dir / "output")
    assert status == 0, f"solver status={status}, expected 0"
    assert obj is not None and not math.isnan(obj)
    # Tight tolerance — the LP optimum is analytical (g2=100 MW, g1=50 MW
    # at pmin, 4-hour block, no scaling applied to the reported value).
    assert obj == pytest.approx(_EXPECTED_OBJ_VALUE, rel=1e-6)
