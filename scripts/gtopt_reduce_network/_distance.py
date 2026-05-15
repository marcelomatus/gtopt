# SPDX-License-Identifier: BSD-3-Clause
"""Bus-to-bus distance metrics for clustering.

Three metrics are exposed; all return a square ``(n_bus, n_bus)`` numpy
array indexed by the order in :class:`LineGraph.bus_uids`.

* ``reactance_shortest_path_matrix`` — sum of equivalent ``|X|`` along
  the cheapest path; cheapest, no matrix factorisation.
* ``zbus_distance_matrix`` — ``d_ij = Z_ii + Z_jj − 2 Z_ij`` from the
  inverse of the reduced bus-susceptance matrix; per connected
  component.
* ``ptdf_distance_matrix`` — Euclidean distance between bus-PTDF
  columns (``‖PTDF[:, i] − PTDF[:, j]‖``); the most LP-relevant.
"""

from __future__ import annotations

import numpy as np
from scipy.sparse.csgraph import connected_components, dijkstra

from gtopt_reduce_network._topology import (
    LineGraph,
    build_admittance,
    build_undirected_adjacency,
)


def reactance_shortest_path_matrix(graph: LineGraph) -> np.ndarray:
    """Symmetric all-pairs shortest path with edge weights ``|X|``.

    Disconnected pairs receive ``np.inf``; clustering callers must
    partition by component before calling.
    """
    if graph.n_buses == 0:
        return np.zeros((0, 0))
    adj = build_undirected_adjacency(graph)
    dist = dijkstra(adj, directed=False)
    return np.asarray(dist, dtype=float)


def zbus_distance_matrix(graph: LineGraph) -> np.ndarray:
    """Per-component Z-bus distance ``d_ij = Z_ii + Z_jj − 2 Z_ij``.

    Each component picks its lowest-uid bus as the reference (slack);
    inter-component pairs receive ``np.inf``.
    """
    n = graph.n_buses
    if n == 0:
        return np.zeros((0, 0))
    bbus = build_admittance(graph).toarray()
    dist = np.full((n, n), np.inf, dtype=float)
    n_comp, labels = connected_components(build_undirected_adjacency(graph))
    for cid in range(n_comp):
        comp_idx = np.where(labels == cid)[0]
        if comp_idx.size == 1:
            i = int(comp_idx[0])
            dist[i, i] = 0.0
            continue
        # Pick the lowest-uid bus as reference.
        ref_local = int(np.argmin([graph.bus_uids[i] for i in comp_idx]))
        ref_global = int(comp_idx[ref_local])
        non_ref = comp_idx[comp_idx != ref_global]
        b_red = bbus[np.ix_(non_ref, non_ref)]
        try:
            z_red = np.linalg.inv(b_red)
        except np.linalg.LinAlgError:
            # Component degenerate (parallel-line cancel after merge?) —
            # leave inf so cluster only reaches via shortest-path metric.
            continue
        # Z full has zero row/col at the reference.
        z_full = np.zeros((comp_idx.size, comp_idx.size), dtype=float)
        non_ref_local = np.array([k for k, g in enumerate(comp_idx) if g != ref_global])
        for i_local, i_g in enumerate(non_ref_local):
            for j_local, j_g in enumerate(non_ref_local):
                z_full[i_g, j_g] = z_red[i_local, j_local]
        for i in range(comp_idx.size):
            for j in range(comp_idx.size):
                d = z_full[i, i] + z_full[j, j] - 2.0 * z_full[i, j]
                dist[comp_idx[i], comp_idx[j]] = d
    np.fill_diagonal(dist, 0.0)
    return dist


def ptdf_distance_matrix(graph: LineGraph) -> np.ndarray:
    """Euclidean distance between bus PTDF columns.

    PTDF is built per component using the lowest-uid bus as slack; the
    reference column is included as zeros (matches the
    ``gtopt_marginal_units/_zones.build_ptdf`` convention).
    """
    n = graph.n_buses
    if n == 0:
        return np.zeros((0, 0))
    ptdf = _build_ptdf_matrix(graph)
    # Pairwise column-Euclidean.
    diffs = ptdf[:, :, None] - ptdf[:, None, :]
    return np.sqrt(np.sum(diffs * diffs, axis=0))


def _build_ptdf_matrix(graph: LineGraph) -> np.ndarray:
    """Dense PTDF (``n_lines × n_buses``); zero block-diag for islands."""
    n_buses = graph.n_buses
    n_lines = graph.n_lines
    ptdf = np.zeros((n_lines, n_buses), dtype=float)
    if n_lines == 0:
        return ptdf
    bbus = build_admittance(graph).toarray()
    inv_x = 1.0 / graph.line_x
    n_comp, labels = connected_components(build_undirected_adjacency(graph))
    for cid in range(n_comp):
        comp_idx = np.where(labels == cid)[0]
        if comp_idx.size <= 1:
            continue
        ref_local = int(np.argmin([graph.bus_uids[i] for i in comp_idx]))
        ref_global = int(comp_idx[ref_local])
        non_ref = comp_idx[comp_idx != ref_global]
        b_red = bbus[np.ix_(non_ref, non_ref)]
        try:
            b_red_inv = np.linalg.inv(b_red)
        except np.linalg.LinAlgError:
            continue
        # For every line whose endpoints are inside this component:
        comp_set = set(int(c) for c in comp_idx)
        for li in range(n_lines):
            a = int(graph.line_a[li])
            b = int(graph.line_b[li])
            if a not in comp_set or b not in comp_set:
                continue
            inj = np.zeros(non_ref.size, dtype=float)
            for k, g in enumerate(non_ref):
                if int(g) == a:
                    inj[k] = inv_x[li]
                elif int(g) == b:
                    inj[k] = -inv_x[li]
            ptdf_row = inj @ b_red_inv
            for k, g in enumerate(non_ref):
                ptdf[li, int(g)] = ptdf_row[k]
            # Reference bus column stays zero.
    return ptdf
