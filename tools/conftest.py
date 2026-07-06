# SPDX-License-Identifier: BSD-3-Clause
"""pytest configuration for the ``tools/`` test suite.

Caps xdist ``-n auto`` to a single worker when the ambient default solver is a
GPU first-order backend (cuOpt).  The GPU-solving tests here (``ucjl2gtopt``)
spawn ``gtopt`` subprocesses that solve on the one GPU; under many parallel
xdist workers they oversubscribe it, starving the pinned CPLEX-MIP subprocesses
into suboptimal incumbents and loosening cuOpt's own PDLP convergence — flaky
failures that vanish when the suite runs serially (verified: ``-n 1`` and
``-p no:xdist`` are green under cuOpt).  Exact CPU solvers keep full ``-n auto``
parallelism, so normal CI is unaffected.
"""

from __future__ import annotations

import os

# Kept in sync with tools/test_ucjl2gtopt.py::_FIRST_ORDER_SOLVERS and
# tools/validate_sddp_output.py::FIRST_ORDER_SOLVERS.
_FIRST_ORDER_SOLVERS = frozenset({"cuopt"})


def pytest_xdist_auto_num_workers(config) -> int | None:  # noqa: ARG001
    """xdist hook: resolve ``-n auto`` to 1 worker under a first-order solver."""
    if os.environ.get("GTOPT_SOLVER", "") in _FIRST_ORDER_SOLVERS:
        return 1
    return None
