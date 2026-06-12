# SPDX-License-Identifier: BSD-3-Clause
"""Zone partition + per-component PTDF.

Master plan §4.3 (zone partition) and §4.7 R1 (PTDF estimate when
flows are absent). The connected-components algorithm is a
hand-rolled BFS — per the python-reviewer P1 finding, networkx is
overkill for graphs of ≤ 1000 nodes.

The PTDF builder is split per *connected component* of the
topology graph so islanded networks don't yield a singular
reduced-Laplacian. Per the lp-numerics P0.3 fix.
"""

from __future__ import annotations

from typing import Iterable

import numpy as np

from gtopt_canonical_feed import Line, Topology
from gtopt_marginal_units.errors import InputValidationError


# ---------------------------------------------------------------------------
# Connected-components partition.
# ---------------------------------------------------------------------------


def partition_zones(
    topology: Topology,
    saturated_line_uids: Iterable[int] = (),
    inactive_line_uids: Iterable[int] = (),
) -> dict[int, int]:
    """Partition buses into zones based on which lines remain active.

    A line is "removed" from the topology graph if it appears in
    either ``saturated_line_uids`` (a saturated line splits the
    network into congestion sub-zones) or ``inactive_line_uids`` (a
    line with ``active=False`` in this stage).

    Args:
        topology: full topology including buses and all lines.
        saturated_line_uids: line UIDs to drop because they are at
            their thermal limit (with non-zero flow dual in the
            simulated case, or via CDC declaration / flow-only / PTDF
            inference in the real case).
        inactive_line_uids: line UIDs to drop because the line is
            tagged ``active=False`` in this stage's catalogue.

    Returns:
        {bus_uid: zone_id}, with zone_id assigned in lowest-bus-uid
        order so the same topology with the same saturation pattern
        always produces the same partition.
    """
    drop = set(saturated_line_uids) | set(inactive_line_uids)
    adj: dict[int, list[int]] = {b.uid: [] for b in topology.buses}
    for line in topology.lines:
        if line.uid in drop or not line.active:
            continue
        if line.bus_a_uid not in adj or line.bus_b_uid not in adj:
            continue
        adj[line.bus_a_uid].append(line.bus_b_uid)
        adj[line.bus_b_uid].append(line.bus_a_uid)

    zone_of: dict[int, int] = {}
    next_zone = 0
    # Sort by bus uid for determinism.
    for start in sorted(adj):
        if start in zone_of:
            continue
        # BFS from `start`.
        stack = [start]
        while stack:
            node = stack.pop()
            if node in zone_of:
                continue
            zone_of[node] = next_zone
            stack.extend(neigh for neigh in adj[node] if neigh not in zone_of)
        next_zone += 1
    return zone_of


def zones_to_components(zone_of: dict[int, int]) -> list[list[int]]:
    """Convert a {bus_uid: zone_id} map into a list of per-zone bus
    lists, sorted by zone_id and within-zone by bus_uid."""
    comps: dict[int, list[int]] = {}
    for bus_uid, zid in zone_of.items():
        comps.setdefault(zid, []).append(bus_uid)
    return [sorted(comps[z]) for z in sorted(comps)]


# ---------------------------------------------------------------------------
# Phantom-bus detection (#525)
# ---------------------------------------------------------------------------
#
# PLEXOS BESS use the **source_generator pattern** — each battery has a
# synthetic internal bus (``BAT_<NAME>_int_bus``) with a single fake
# generator (``BAT_<NAME>_LOAD`` or ``BAT_<NAME>_gen``) and 0 incident
# lines.  The battery's energy balance is enforced by separate
# ``Battery`` LP constraints, not by line flow.
#
# These phantom buses fool the merit-unit classifier: 0 lines → bus is
# its own island; only synthetic gens → no merit-eligible candidate →
# ``formula_kind = "unattributed"``.  But the LP itself gives perfectly
# good non-zero LMP at these buses (derived from the battery's energy-
# balance dual).  The right classification is ``hydro_marginal``
# (storage marginal) — em=0 by physics, recomputed_lmp = LP LMP.


_PHANTOM_BUS_NAME_PATTERNS: tuple[str, ...] = (
    "_int_bus",  # PLEXOS source_generator phantom bus
    "_INT_BUS",
)
_PHANTOM_GEN_NAME_FRAGMENTS: tuple[str, ...] = (
    "_LOAD",
    "_load",
)
_PHANTOM_GEN_PREFIX: str = "BAT_"


def is_phantom_bus(bus_name: str, gens_on_bus: list) -> bool:
    """True when ``bus_name`` matches a phantom-bus pattern OR the only
    generators on it are synthetic battery-source units.

    Phantom-bus signal:
      * bus name ends with ``_int_bus`` / ``_INT_BUS`` (PLEXOS
        source_generator internal bus convention), OR
      * every generator on the bus matches ``BAT_*_LOAD`` /
        ``BAT_*_gen`` (synthetic charge-demand / discharge-output
        wrappers from the PLEXOS BESS pattern).

    Used by ``_zone_results_from_lp_duals`` to classify the bus's
    zone as a storage marginal (``hydro_marginal``) rather than
    ``unattributed`` when no merit-eligible candidate exists.
    """
    if any(bus_name.endswith(p) for p in _PHANTOM_BUS_NAME_PATTERNS):
        return True
    if not gens_on_bus:
        return False
    # All generators on the bus are synthetic battery-source wrappers
    return all(
        str(g.name).startswith(_PHANTOM_GEN_PREFIX)
        and any(frag in str(g.name) for frag in _PHANTOM_GEN_NAME_FRAGMENTS + ("_gen",))
        for g in gens_on_bus
    )


# ---------------------------------------------------------------------------
# PTDF builder (lp-numerics P0.3 fix — per-component).
# ---------------------------------------------------------------------------


def build_ptdf(
    topology: Topology,
    *,
    require_reactance: bool = True,
) -> tuple[np.ndarray, list[int], list[int]]:
    """Construct the PTDF (Power Transfer Distribution Factor)
    matrix for the topology, **per connected component**.

    PTDF[l, b] = ∂f_l / ∂(net injection at b) at the topology's
    reference bus (lowest-uid bus per component).

    Returns:
        ptdf: dense float matrix of shape (n_lines, n_buses).
        line_uids: line UIDs in row order.
        bus_uids: bus UIDs in column order.

    Raises:
        InputValidationError: if any active line in any component
            has a missing or non-positive reactance and
            ``require_reactance`` is True. Per the master plan §4.7
            R1, "uniform reactance fallback" is *not* a v1 option —
            we abort rather than emit silently-wrong zones.
    """
    bus_uids = sorted(b.uid for b in topology.buses)
    bus_idx = {u: i for i, u in enumerate(bus_uids)}
    active_lines = [ln for ln in topology.lines if ln.active and ln.uid is not None]

    if require_reactance:
        bad = [
            ln.uid for ln in active_lines if ln.reactance is None or ln.reactance <= 0
        ]
        if bad:
            raise InputValidationError(
                "PTDF requires a positive reactance on every active line; "
                f"missing or non-positive on lines: {bad}. "
                "Pass --zone-mode physical to skip flow-based partitioning, "
                "or fix the topology's reactance values."
            )

    line_uids = [ln.uid for ln in active_lines]
    n_lines = len(line_uids)
    n_buses = len(bus_uids)
    ptdf = np.zeros((n_lines, n_buses), dtype=float)

    # Component partition (using the topology with all lines active —
    # we want the *physical* components, not a saturation-induced cut).
    zone_of = partition_zones(topology)

    # Group buses and lines per component.
    comps_buses: dict[int, list[int]] = {}
    for u, z in zone_of.items():
        comps_buses.setdefault(z, []).append(u)
    comps_lines: dict[int, list[Line]] = {}
    for ln in active_lines:
        z_a = zone_of.get(ln.bus_a_uid)
        z_b = zone_of.get(ln.bus_b_uid)
        if z_a is None or z_b is None or z_a != z_b:
            # Inter-component "line" (must not happen for a connected
            # component) — just skip; PTDF row stays zero.
            continue
        comps_lines.setdefault(z_a, []).append(ln)

    # Build PTDF block-diagonal — one solve per component.
    for zid, comp_buses in comps_buses.items():
        comp_buses_sorted = sorted(comp_buses)
        # Reference bus = lowest uid in the component.
        ref_bus = comp_buses_sorted[0]
        # Index of each comp_bus in the global bus_uids array.
        local_to_global = [bus_idx[u] for u in comp_buses_sorted]

        # Drop the reference bus column from the local indexing
        # (reduced bus-susceptance matrix B is non-singular).
        non_ref_local = [i for i, u in enumerate(comp_buses_sorted) if u != ref_bus]

        nlocal = len(comp_buses_sorted)
        nbar = nlocal - 1
        if nbar == 0:
            # Single-bus component: no transfer paths, nothing to do.
            continue

        # Build B (reduced) and X (line-bus incidence weighted by 1/x).
        comp_lines_local = comps_lines.get(zid, [])
        if not comp_lines_local:
            continue
        local_index = {u: i for i, u in enumerate(comp_buses_sorted)}
        # B-matrix in local coordinates including the reference.
        b_full = np.zeros((nlocal, nlocal), dtype=float)
        for ln in comp_lines_local:
            ia = local_index[ln.bus_a_uid]
            ib = local_index[ln.bus_b_uid]
            assert ln.reactance is not None  # require_reactance=True checked above
            inv_x = 1.0 / float(ln.reactance)
            b_full[ia, ia] += inv_x
            b_full[ib, ib] += inv_x
            b_full[ia, ib] -= inv_x
            b_full[ib, ia] -= inv_x

        # Reduced B_red = drop ref row and column.
        b_red = b_full[np.ix_(non_ref_local, non_ref_local)]
        try:
            b_red_inv = np.linalg.inv(b_red)
        except np.linalg.LinAlgError as exc:
            raise InputValidationError(
                f"PTDF component {zid} has a singular reduced "
                f"susceptance matrix; check for parallel lines or "
                f"degenerate topology: {exc}"
            ) from exc

        # X-matrix for the component: for each line, e_l = (1/x_l) * (e_a − e_b)
        # in local non-ref coordinates.
        for ln in comp_lines_local:
            try:
                row = line_uids.index(ln.uid)
            except ValueError as exc:
                # A line that's in this component's local list but missing
                # from ``line_uids`` is a topology-vs-PTDF mismatch — the
                # PTDF row would be incomplete, silently corrupting
                # downstream marginal attribution on this line's buses.
                raise InputValidationError(
                    f"line uid={ln.uid} (component {zid}, "
                    f"bus_a={ln.bus_a_uid}, bus_b={ln.bus_b_uid}) is in "
                    f"the component's line list but missing from the "
                    f"global ``line_uids`` index — PTDF would be "
                    f"incomplete. Topology / line array out of sync."
                ) from exc
            assert ln.reactance is not None  # require_reactance=True checked above
            inv_x = 1.0 / float(ln.reactance)
            ia = local_index[ln.bus_a_uid]
            ib = local_index[ln.bus_b_uid]

            # PTDF row in local non-ref coords.
            e_local = np.zeros(nbar, dtype=float)
            if ia in non_ref_local:
                e_local[non_ref_local.index(ia)] += inv_x
            if ib in non_ref_local:
                e_local[non_ref_local.index(ib)] -= inv_x

            ptdf_local = e_local @ b_red_inv  # shape (nbar,)

            # Map back to global column indices: ref column stays 0.
            for j_local, ptdf_val in enumerate(ptdf_local):
                global_col = local_to_global[non_ref_local[j_local]]
                ptdf[row, global_col] = ptdf_val
            # Reference bus column is implicitly zero (slack absorbs
            # the balance), so we don't write it.

    return ptdf, line_uids, bus_uids


def estimate_flows(
    topology: Topology,
    bus_net_injection: dict[int, float],
    *,
    require_reactance: bool = True,
) -> dict[int, float]:
    """Estimate line flows from realised bus net injections via
    f = PTDF · netload.

    Returns {line_uid: flow}. Used by §4.7 R1 step (4) when neither
    CDC declarations nor realised flows are available.
    """
    ptdf, line_uids, bus_uids = build_ptdf(
        topology, require_reactance=require_reactance
    )
    netload = np.array([bus_net_injection.get(u, 0.0) for u in bus_uids], dtype=float)
    flows = ptdf @ netload  # (n_lines,)
    return {u: float(v) for u, v in zip(line_uids, flows)}
