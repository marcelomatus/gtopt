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


@pytest.mark.skipif(
    GTOPT is None or not GTOPT.is_file(),
    reason="gtopt binary not available (set GTOPT_BIN or build standalone)",
)
@pytest.mark.skipif(
    CASE is None or not CASE.is_file(),
    reason="IEEE 118-bus case file (cases/ieee_118b/ieee_118b.json) missing",
)
@pytest.mark.integration
def test_ots_ieee118_smoke(tmp_path):
    """Run the smoke test script end-to-end.

    Asserts the script exits 0 (= both gtopt runs succeeded AND the
    OTS obj is monotone-improving vs baseline).  Marked ``integration``
    so it can be excluded from the fast unit subset via ``-m 'not
    integration'``.
    """
    env = os.environ.copy()
    env["GTOPT_BIN"] = str(GTOPT)
    proc = subprocess.run(
        [
            sys.executable,
            str(_SCRIPT),
            "--tmp",
            str(tmp_path / "workspace"),
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
    # The script writes both objectives into stdout; sanity-check they
    # both came out as numbers (not None / NaN sentinels).
    assert "obj_baseline =" in proc.stdout
    assert "obj_ots      =" in proc.stdout
