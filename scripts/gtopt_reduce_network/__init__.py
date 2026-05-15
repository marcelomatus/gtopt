# SPDX-License-Identifier: BSD-3-Clause
"""gtopt_reduce_network — bus/line reducer + solution projector for gtopt cases.

Produces a smaller electrically-equivalent gtopt JSON case (reducer) plus the
metadata needed to project dispatch results and capacity-expansion outcomes
back onto the original nodal grid (projector).

See ``docs/network_reduction_proposal.md`` for the algorithm and
``/home/marce/.claude/plans/fizzy-cuddling-llama.md`` for the implementation
plan.
"""

from __future__ import annotations

from gtopt_reduce_network._aggregate import aggregate_lines
from gtopt_reduce_network._busmap import (
    AggregatorRow,
    BusmapRow,
    LinemapRow,
    load_aggregator,
    load_busmap,
    load_linemap,
    save_aggregator,
    save_busmap,
    save_linemap,
)
from gtopt_reduce_network._cluster import build_busmap, select_anchors
from gtopt_reduce_network._components import rewrite_component_buses
from gtopt_reduce_network._distance import (
    ptdf_distance_matrix,
    reactance_shortest_path_matrix,
    zbus_distance_matrix,
)
from gtopt_reduce_network._io import (
    Case,
    classify_schedule_field,
    load_case,
    resolve_bus_ref,
    save_case,
)
from gtopt_reduce_network._local_simplify import simplify_local
from gtopt_reduce_network._topology import build_admittance, build_line_graph

__all__ = [
    "AggregatorRow",
    "BusmapRow",
    "Case",
    "LinemapRow",
    "aggregate_lines",
    "build_admittance",
    "build_busmap",
    "build_line_graph",
    "classify_schedule_field",
    "load_aggregator",
    "load_busmap",
    "load_case",
    "load_linemap",
    "ptdf_distance_matrix",
    "reactance_shortest_path_matrix",
    "resolve_bus_ref",
    "rewrite_component_buses",
    "save_aggregator",
    "save_busmap",
    "save_case",
    "save_linemap",
    "select_anchors",
    "simplify_local",
    "zbus_distance_matrix",
]
