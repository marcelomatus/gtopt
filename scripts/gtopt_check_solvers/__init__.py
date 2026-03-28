# SPDX-License-Identifier: BSD-3-Clause
"""gtopt_check_solvers — discover and validate gtopt LP solver plugins.

Public API
----------
The main entry point is
:func:`gtopt_check_solvers.gtopt_check_solvers.main` (registered as the
``gtopt_check_solvers`` console script).

Sub-modules
-----------
:mod:`._binary`        — gtopt binary discovery
:mod:`._solver_tests`  — LP solver test-suite definition and runner
"""

from .gtopt_check_solvers import main

__all__ = ["main"]
