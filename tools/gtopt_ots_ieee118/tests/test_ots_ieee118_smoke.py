# SPDX-License-Identifier: BSD-3-Clause
"""Integration smoke test for OTS on the IEEE 118-bus benchmark.

Validates that the v0..v1.3 LineCommitment LP stack scales to a
118-bus / 186-line case without crashing and produces a monotone-
improving objective vs the no-OTS baseline.

Skipped automatically when (a) the gtopt binary cannot be located,
(b) the IEEE 118-bus case file is missing, or (c) the case JSON
schema lacks the v0 ``line_commitment_array`` member (older binary).

For the LP-relax / MIP / line-limit caveats and the Fisher 2008
golden-value comparison, see ``gtopt_ots_ieee118.py``'s docstring.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


_HERE = Path(__file__).resolve().parent
_SCRIPT = _HERE.parent / "gtopt_ots_ieee118.py"


def _gtopt_binary() -> Path | None:
    """Resolve the gtopt binary.

    Priority order (tests use a stricter resolution than the runtime
    script so an outdated ``~/.local/bin/gtopt`` doesn't shadow a
    freshly-built standalone binary):

      1. ``$GTOPT_BIN`` env var (explicit override).
      2. ``<repo>/build/standalone/gtopt`` (in-tree build).
      3. ``shutil.which("gtopt")`` on PATH.
    """
    env = os.environ.get("GTOPT_BIN", "").strip()
    if env:
        return Path(env)
    # In-tree build first.
    for parent in (*_HERE.parents,):
        cand = parent / "build" / "standalone" / "gtopt"
        if cand.is_file():
            return cand
    which = shutil.which("gtopt")
    if which is not None:
        return Path(which)
    return None


def _case_file() -> Path | None:
    """Locate ``cases/ieee_118b/ieee_118b.json`` relative to project root."""
    for parent in (*_HERE.parents,):
        cand = parent / "cases" / "ieee_118b" / "ieee_118b.json"
        if cand.is_file():
            return cand
    return None


GTOPT = _gtopt_binary()
CASE = _case_file()


# Top-10 most-utilised lines at the 0.02× baseline (≈ 200 MW caps).
# Pinned in the test so the MIP is small + reproducible.
_CANDIDATES = "l26_30,l38_65,l89_92,t8_5,t68_69,t38_37,t65_66,t116_68,l8_9,l9_10"


@pytest.mark.skipif(
    GTOPT is None or not GTOPT.is_file(),
    reason="gtopt binary not available (set GTOPT_BIN or build standalone)",
)
@pytest.mark.skipif(
    CASE is None or not CASE.is_file(),
    reason="IEEE 118-bus case file (cases/ieee_118b/ieee_118b.json) missing",
)
@pytest.mark.integration
def test_ots_ieee118_mip_finds_positive_savings(tmp_path):
    """Run the IEEE 118-bus MIP OTS solve on a 10-line candidate set.

    Asserts (a) both gtopt runs succeed, (b) ``obj_ots < obj_baseline``
    by at least 0.1 %.  The MIP with 10 binaries + 300 s time limit
    reproducibly finds ~0.4 % savings on this case (Fisher 2008's
    all-line MIP would reach 25 % but is hours of solve time).

    Marked ``integration`` so it can be excluded from the fast unit
    subset via ``-m 'not integration'``.
    """
    env = os.environ.copy()
    env["GTOPT_BIN"] = str(GTOPT)
    proc = subprocess.run(
        [
            sys.executable,
            str(_SCRIPT),
            "--tmp",
            str(tmp_path / "workspace"),
            "--time-limit",
            "300",
            "--mip-gap",
            "0.01",
            "--candidate-lines",
            _CANDIDATES,
        ],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    print(proc.stdout)
    print(proc.stderr, file=sys.stderr)
    assert proc.returncode == 0, (
        f"gtopt_ots_ieee118.py exited {proc.returncode}; see captured "
        "stdout/stderr above for the gtopt error"
    )

    # Extract the two objectives the script printed.
    obj_baseline = obj_ots = None
    for line in proc.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("obj_baseline ="):
            obj_baseline = float(stripped.split("=", 1)[1].strip())
        elif stripped.startswith("obj_ots") and "=" in stripped:
            obj_ots = float(stripped.split("=", 1)[1].strip())
    assert obj_baseline is not None, "obj_baseline not found in stdout"
    assert obj_ots is not None, "obj_ots not found in stdout"

    # Monotonicity: OTS can only improve over baseline.
    assert obj_ots <= obj_baseline + 1e-6, (
        f"OTS obj {obj_ots} > baseline {obj_baseline}; MIP likely "
        "returned a sub-optimal incumbent."
    )

    # Golden value: the analytical LP floor.
    # pp2gtopt collapses pglib-opf's quadratic ``cp1·P + cp2·P²``
    # cost to its linear part (cp1 only).  ``ieee_118b`` has 19
    # generators at $20/MWh with total capacity 6 466 MW > 4 242 MW
    # demand, so the gtopt LP optimum is bounded BELOW by
    # ``$20 × 4 242 = $84 840``.  Baseline sits just above this
    # (= floor + congestion penalty); OTS pushes it back down to
    # the floor exactly.  See cases/ieee_118b/README.md "Why gtopt ≠
    # pandapower" for the cp2 discussion.
    GTOPT_LP_FLOOR = 20.0 * 4242.0  # = 84 840 $/h

    assert obj_baseline > GTOPT_LP_FLOOR, (
        f"Baseline obj {obj_baseline} ≤ analytical LP floor "
        f"{GTOPT_LP_FLOOR}; the test fixture has no congestion. "
        "Check that --line-limit-scale 0.02 is being applied."
    )
    assert obj_baseline <= 1.10 * GTOPT_LP_FLOOR, (
        f"Baseline obj {obj_baseline} > 110 % of analytical floor "
        f"{GTOPT_LP_FLOOR}; the cost data may have changed.  "
        "If pp2gtopt now imports cp2 quadratic costs, the floor "
        "needs to be re-derived from the new effective marginal."
    )

    # OTS should reach the analytical floor exactly: with linearised
    # costs the optimal dispatch is "all cheap gens up to demand",
    # and OTS removes any topological obstacle to that.
    assert abs(obj_ots - GTOPT_LP_FLOOR) <= 0.01, (
        f"OTS obj {obj_ots} differs from analytical LP floor "
        f"{GTOPT_LP_FLOOR} by ${abs(obj_ots - GTOPT_LP_FLOOR):.4f}; "
        "expected exact match within solver tolerance.  Either "
        "the MIP returned a sub-optimal incumbent or the cost data "
        "changed."
    )

    # Derived metric: ≥ 90 % of congestion cost eliminated.
    congestion_cost = obj_baseline - GTOPT_LP_FLOOR
    eliminated_pct = (obj_baseline - obj_ots) / congestion_cost
    assert eliminated_pct >= 0.90, (
        f"OTS eliminated only {eliminated_pct * 100:.1f} % of the "
        f"${congestion_cost:.2f} congestion cost; expected ≥ 90 %.  "
        "Either the OTS LP build is silently skipping LineCommitment "
        "(check chronological gate, kirchhoff_mode, method=monolithic) "
        "or the MIP solver returned a sub-optimal incumbent."
    )
