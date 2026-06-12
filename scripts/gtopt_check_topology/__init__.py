# SPDX-License-Identifier: BSD-3-Clause
"""gtopt_check_topology -- preflight network topology analyzer.

Public API
----------
The main entry point is :func:`gtopt_check_topology.main.main` (registered as
the ``gtopt_check_topology`` console script).

Sub-modules
-----------
:mod:`._analyzer` -- pure-data graph analysis (islands, cycles, bridges,
                     danger score, SOS1 candidates)
:mod:`._render`   -- rich-based console renderer and JSON report writer
:mod:`.main`      -- argparse CLI
"""

from gtopt_check_topology._analyzer import (
    DC_REACTANCE_TOL,
    AnalysisReport,
    CycleInfo,
    LineRef,
    NetworkGraph,
    analyse_planning,
    build_network_graph,
    cycle_danger_score,
)
from gtopt_check_topology._render import (
    render_console_report,
    report_to_json,
    sos1_candidates_payload,
)
from gtopt_check_topology.main import main

__all__ = [
    "DC_REACTANCE_TOL",
    "AnalysisReport",
    "CycleInfo",
    "LineRef",
    "NetworkGraph",
    "analyse_planning",
    "build_network_graph",
    "cycle_danger_score",
    "main",
    "render_console_report",
    "report_to_json",
    "sos1_candidates_payload",
]
