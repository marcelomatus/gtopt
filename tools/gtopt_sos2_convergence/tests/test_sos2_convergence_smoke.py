# SPDX-License-Identifier: BSD-3-Clause
"""Integration smoke test for SOS2 L-secant convergence on
IEEE 14-bus (Coffrin reference case, issue #504).

Validates the **chord-tightening invariance** property of gtopt's
L-secant + SOS2 line-loss approximation:

  * L = 1 reproduces Coffrin & Van Hentenryck (2014)'s classic
    single-secant linear loss approximation.
  * L = 2 + SOS2 tightens the chord upper bound to a piecewise-
    linear over-approximation.
  * Because gtopt's LP picks ``ℓ_line`` at the **MAX of K tangent
    lower bounds** (K-dependent, NOT L-dependent), the LP-observed
    objective and total network loss are **invariant under the L
    sweep** (within solver tolerance).

L ≥ 3 is **deliberately not** in the sweep — see
``gtopt_sos2_convergence.py``'s "SEGMENT-FORMULATION TRAP" docstring
section for why (canonical Beale–Tomlin SOS2 caps ``|f| ≤ 2w =
2·envelope/L`` which is below tmax for L ≥ 3 in the segment form).

Skipped automatically when (a) the gtopt binary cannot be located
or (b) the IEEE 14-bus case file is missing.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


_HERE = Path(__file__).resolve().parent
_SCRIPT = _HERE.parent / "gtopt_sos2_convergence.py"


def _gtopt_binary() -> Path | None:
    """Resolve the gtopt binary.

    Priority order matches the OTS test (in-tree build BEFORE
    ``shutil.which`` to avoid stale ``~/.local/bin/gtopt``).
    """
    env = os.environ.get("GTOPT_BIN", "").strip()
    if env:
        return Path(env)
    for parent in (*_HERE.parents,):
        cand = parent / "build" / "standalone" / "gtopt"
        if cand.is_file():
            return cand
    which = shutil.which("gtopt")
    return Path(which) if which else None


def _case_file() -> Path | None:
    """Locate ``cases/ieee_14b/ieee_14b.json``."""
    for parent in (*_HERE.parents,):
        cand = parent / "cases" / "ieee_14b" / "ieee_14b.json"
        if cand.is_file():
            return cand
    return None


GTOPT = _gtopt_binary()
CASE = _case_file()


def _parse_table_row(line: str) -> tuple[int, float, float] | None:
    """Parse one ``L  SOS2  obj  loss  loss/demand  Δobj`` row.

    Returns ``(L, obj, loss)`` or ``None`` if the line is not a
    data row.
    """
    parts = line.split()
    if len(parts) < 6:
        return None
    try:
        L = int(parts[0])
    except ValueError:
        return None
    if parts[1] != "yes":
        return None
    try:
        obj = float(parts[2])
        loss = float(parts[3])
    except ValueError:
        return None
    return L, obj, loss


@pytest.mark.skipif(
    GTOPT is None or not GTOPT.is_file(),
    reason="gtopt binary not available (set GTOPT_BIN or build standalone)",
)
@pytest.mark.skipif(
    CASE is None or not CASE.is_file(),
    reason="IEEE 14-bus case file (cases/ieee_14b/ieee_14b.json) missing",
)
@pytest.mark.integration
def test_sos2_convergence_invariance_L1_L2(tmp_path):
    """L=1 vs L=2 + SOS2 must produce identical obj + loss.

    This is the chord-tightening invariance property: the LP picks
    ``ℓ_line = max(tangent_k(f_line))`` regardless of the chord
    upper bound, so increasing L tightens an inactive constraint
    and does not move the solution.

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
            "--L-values",
            "1,2",
        ],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    print(proc.stdout)
    print(proc.stderr, file=sys.stderr)
    assert proc.returncode == 0, (
        f"gtopt_sos2_convergence.py exited {proc.returncode}; "
        "see captured stdout/stderr above for the gtopt error"
    )

    rows = []
    for line in proc.stdout.splitlines():
        row = _parse_table_row(line)
        if row is not None:
            rows.append(row)
    assert len(rows) == 2, f"Expected 2 data rows (L=1, L=2); got {len(rows)}: {rows}"
    L1_row = next((r for r in rows if r[0] == 1), None)
    L2_row = next((r for r in rows if r[0] == 2), None)
    assert L1_row is not None, "L=1 row missing"
    assert L2_row is not None, "L=2 row missing"

    _, obj_L1, loss_L1 = L1_row
    _, obj_L2, loss_L2 = L2_row

    # Invariance: the LP picks max-tangent for ℓ regardless of the
    # chord upper bound, so obj and loss must match within solver
    # tolerance (CPLEX default optimality ~ 1e-6 relative).
    assert abs(obj_L2 - obj_L1) <= 1e-4 * max(1.0, abs(obj_L1)), (
        f"L=2 obj {obj_L2:.6f} differs from L=1 obj {obj_L1:.6f} by "
        f"{abs(obj_L2 - obj_L1):.6f} > 1e-4 × max(1, |obj_L1|).  "
        "The L-secant chord upper bound should be inactive at the "
        "tangent lower bound, so doubling L must not move the LP "
        "optimum.  Either the tangent rows were dropped or the "
        "SOS2 fill-order is rejecting feasible flows."
    )
    assert abs(loss_L2 - loss_L1) <= 1e-3 * max(1.0, abs(loss_L1)), (
        f"L=2 total loss {loss_L2:.4f} MWh differs from L=1 loss "
        f"{loss_L1:.4f} MWh by more than 0.1 %.  Same invariance "
        "as above."
    )

    # Sanity: positive losses (~ 4 % of demand on IEEE 14).
    assert loss_L1 > 0, "L=1 reported zero losses — resistance not injected"
    assert loss_L1 < 1000.0, f"L=1 loss {loss_L1:.1f} MWh > 1000 — unphysical"


@pytest.mark.skipif(
    GTOPT is None or not GTOPT.is_file(),
    reason="gtopt binary not available (set GTOPT_BIN or build standalone)",
)
@pytest.mark.skipif(
    CASE is None or not CASE.is_file(),
    reason="IEEE 14-bus case file (cases/ieee_14b/ieee_14b.json) missing",
)
@pytest.mark.integration
def test_sos2_segment_trap_at_L4(tmp_path):
    """L=4 + SOS2 must hit the segment-cap trap: line 1 saturates at
    ``tmax/2``, demand-fail kicks in, obj jumps ≥ 2× over baseline.

    This pins the **known bug** in the segment formulation so a
    future SOS2 reformulation (lambda-form or fill-order binaries)
    will fail this test and force a re-think of the assertion.
    """
    env = os.environ.copy()
    env["GTOPT_BIN"] = str(GTOPT)
    proc = subprocess.run(
        [
            sys.executable,
            str(_SCRIPT),
            "--tmp",
            str(tmp_path / "workspace"),
            "--L-values",
            "1",
            "--probe-sos2-trap",
        ],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    print(proc.stdout)
    print(proc.stderr, file=sys.stderr)
    # The script's --probe-sos2-trap path asserts obj > 2× L=1, so
    # exit code 0 means the trap reproduced as expected.  Non-zero
    # would mean the trap is no longer triggered (e.g. SOS2 was
    # reformulated) and the script's assertion failed.
    assert proc.returncode == 0, (
        f"gtopt_sos2_convergence.py exited {proc.returncode}; this "
        "test EXPECTS the L=4 SOS2 trap to fire (obj > 2× L=1).  "
        "If the SOS2 implementation was fixed to use lambda-form or "
        "fill-order binaries, update this test."
    )
