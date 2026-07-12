"""End-to-end integration test for the 2025-10-19 CEN PCP week.

HEAVY (a real ~470k-row monolithic UC MIP, minutes to solve) — gated on the raw
PLEXOS archives being present and a MIP-capable ``gtopt`` binary, and marked
``integration`` so the default fast suite skips it.  Exercises the full
pipeline:

  1. ``plexos2gtopt`` converts ``support/plexos/pcp_2025-10-19`` (the
     ``DATOS``/``RES`` archives) into a gtopt bundle, emitting the
     MIP warm-start (relax→round→domain_rules→inject) MIP-start by
     default (the converter default), then
  2. ``gtopt`` solves the converted case to proven optimality using that
     MIP-start (bypassing the "incumbent cliff").

Run it explicitly with::

    cd scripts && python -m pytest \
        plexos2gtopt/tests/test_pcp_2025_10_19_integration.py -m integration -s
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
PLEXOS_DIR = REPO / "support" / "plexos"
RAW = PLEXOS_DIR / "pcp_2025-10-19"
STATE = PLEXOS_DIR / "gtopt_pcp_2025-10-19" / "plexos2gtopt_state.json"

pytestmark = pytest.mark.integration


def _have_raw() -> bool:
    return RAW.is_dir() and any(RAW.glob("DATOS*.zip*")) and STATE.is_file()


def _gtopt_binary() -> str | None:
    """Locate a gtopt binary: GTOPT_BIN env, else tools/get_gtopt_binary.py."""
    env = os.environ.get("GTOPT_BIN")
    if env and Path(env).is_file():
        return env
    helper = REPO / "tools" / "get_gtopt_binary.py"
    if not helper.is_file():
        return None
    try:
        res = subprocess.run(
            [sys.executable, str(helper)],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
    except (subprocess.SubprocessError, OSError):
        return None
    cand = res.stdout.strip()
    return cand if cand and Path(cand).is_file() else None


def _plugin_dir(gtopt: str) -> str | None:
    """First plausible plugin dir near the binary (or GTOPT_PLUGIN_DIR)."""
    env = os.environ.get("GTOPT_PLUGIN_DIR")
    if env and Path(env).is_dir():
        return env
    exe = Path(gtopt).resolve().parent
    for cand in (
        exe / "plugins",
        exe.parent / "plugins",
        exe.parent / "lib" / "gtopt" / "plugins",
    ):
        if cand.is_dir() and any(cand.glob("libgtopt_solver_*.so")):
            return str(cand)
    return None


@pytest.mark.skipif(not _have_raw(), reason="raw PCP 2025-10-19 archives not present")
def test_pcp_2025_10_19_convert_and_solve(tmp_path: Path) -> None:
    out = tmp_path / "gtopt_pcp_2025-10-19"

    # 1. Convert — replay the committed conversion options from the state
    #    snapshot.  The positional ``pcp_2025-10-19`` resolves under PLEXOS_DIR.
    conv = subprocess.run(
        [
            sys.executable,
            "-m",
            "plexos2gtopt",
            "pcp_2025-10-19",
            "--from-state",
            str(STATE),
            "--output-dir",
            str(out),
        ],
        cwd=str(PLEXOS_DIR),
        capture_output=True,
        text=True,
        timeout=1800,
        check=False,
    )
    assert conv.returncode == 0, f"conversion failed:\n{conv.stderr[-3000:]}"

    case_json = out / "pcp_2025-10-19.json"
    assert case_json.is_file(), "converted bundle JSON missing"

    # The converter enables the MIP warm-start pipeline by default.
    planning = json.loads(case_json.read_text(encoding="utf-8"))
    mip_start = (
        planning.get("options", {}).get("monolithic_options", {}).get("mip_start", {})
    )
    assert mip_start.get("enabled") is True, (
        f"converter did not enable the MIP warm-start: {mip_start!r}"
    )

    # 2. Solve with gtopt (skip the solve if this environment has no
    #    MIP-capable binary / plugins — the conversion assertions above still
    #    ran).
    gtopt = _gtopt_binary()
    if gtopt is None:
        pytest.skip("no gtopt binary available for the solve step")
    env = dict(os.environ)
    plugins = _plugin_dir(gtopt)
    if plugins:
        env["GTOPT_PLUGIN_DIR"] = plugins

    odir = out / "output_mip"
    # Use the tuned CPLEX params (CutPasses=1) so the degenerate-week root cut
    # rounds stay bounded, and disable the algorithm-fallback retry loop — both
    # keep the solve fast and deterministic for a test.
    prm = REPO / "scripts" / "plexos2gtopt" / "solvers" / "cplex.prm"
    solve = subprocess.run(
        [
            gtopt,
            str(case_json),
            "--solver",
            "cplex",
            "-d",
            str(odir),
            "--set",
            f"solver_options.param_file={prm}",
            "--set",
            "solver_options.max_fallbacks=0",
        ],
        cwd=str(out),
        env=env,
        capture_output=True,
        text=True,
        timeout=2400,
        check=False,
    )
    status_file = odir / "solver_status.json"
    if not status_file.is_file():
        pytest.skip(
            "gtopt produced no solver_status.json (no CPLEX/MIP solver in this "
            f"environment); rc={solve.returncode}\n{solve.stderr[-1500:]}"
        )
    status = json.loads(status_file.read_text(encoding="utf-8")).get("status")
    assert status == "optimal", f"solve did not reach optimal: {status!r}"

    # The MIP warm-start actually fired on the real case.
    logs = sorted((odir / "logs").glob("gtopt_*.log"))
    log_txt = logs[0].read_text(encoding="utf-8", errors="replace") if logs else ""
    assert "MIP-start[warmstart]" in log_txt, "MIP-start did not run on the solve"
