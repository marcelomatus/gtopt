# SPDX-License-Identifier: BSD-3-Clause
"""NetworkX partition backend: Louvain seed + iterative min-cut refinement.

Alternative to the HAC/electrical-distance clustering in ``_cluster.py``,
selected with ``ReduceConfig.partition = "louvain-mincut"``:

1. Build a capacity-weighted (or susceptance-weighted) bus graph.
2. Seed communities per connected component with
   :func:`networkx.community.louvain_communities`.
3. Iteratively adjust the community count to the per-component target K:

   * while above K, **merge** the two communities with the strongest
     inter-community coupling (their boundary is the least
     congestion-relevant one to erase);
   * while below K, **split** the community with the worst internal
     bottleneck — the one whose attached corridor capacity most exceeds
     its internal Stoer–Wagner min-cut — along that min-cut.

The split rule attacks the known weakness of one-shot clustering:
intra-cluster bottlenecks vanish from the reduced model (corridor
capacities are summed across members), making the reduced network
optimistic about transfers and biasing a reduced-network unit-commitment
toward under-commitment in import-constrained pockets.  Spending the bus
budget on the worst internal bottlenecks keeps the congestion pattern —
which is what decides commitment — as faithful as K allows.

NetworkX is imported lazily (same pattern as ``gtopt_diagram``).
"""

from __future__ import annotations

import logging
from typing import Any, Iterable

from gtopt_reduce_network._busmap import BusmapRow
from gtopt_reduce_network._topology import LineGraph

logger = logging.getLogger(__name__)

_WEIGHT_MODES = ("capacity", "susceptance")

# Relative floor applied to zero/negative edge weights so that
# zero-capacity (or unbounded-capacity-encoded-as-0) ties still count as
# connectivity for community detection and min-cut, without dominating.
_WEIGHT_FLOOR_REL = 1e-6


def build_busmap_nx(
    graph: LineGraph,
    *,
    target_buses: int,
    anchors: Iterable[int],
    weight_mode: str = "capacity",
    seed: int = 0,
) -> list[BusmapRow]:
    """Cluster ``graph`` into ``target_buses`` groups via Louvain + min-cut.

    Semantics mirror :func:`_cluster.build_busmap`: cluster ids are bus
    uids (anchor preferred, else smallest member uid); anchors are
    guaranteed their own cluster (splitting an anchor out may overshoot
    K slightly, exactly like the HAC path); disconnected components are
    clustered separately with a proportional share of K.
    """
    if weight_mode not in _WEIGHT_MODES:
        raise ValueError(
            f"weight_mode must be one of {_WEIGHT_MODES}, got {weight_mode!r}"
        )
    import networkx as nx  # noqa: PLC0415

    n = graph.n_buses
    if n == 0:
        return []
    anchor_set = set(int(a) for a in anchors) & set(graph.bus_uids)
    if target_buses >= n:
        return [
            BusmapRow(original_bus_uid=u, cluster_bus_uid=u) for u in graph.bus_uids
        ]

    g = _build_weighted_graph(graph, weight_mode)

    communities: list[set[int]] = []
    components = sorted(nx.connected_components(g), key=min)
    for comp in components:
        comp_uids = sorted(int(u) for u in comp)
        comp_anchors = [u for u in comp_uids if u in anchor_set]
        comp_k = max(
            len(comp_anchors),
            int(round(target_buses * (len(comp_uids) / n))),
            1,
        )
        comp_k = min(comp_k, len(comp_uids))
        sub = g.subgraph(comp_uids)
        comms = _partition_component(sub, comp_k, seed)
        comms = _split_shared_anchor_communities(comms, comp_anchors)
        communities.extend(comms)

    rep_rows: list[BusmapRow] = []
    for members in communities:
        anchors_in = sorted(u for u in members if u in anchor_set)
        rep = anchors_in[0] if anchors_in else min(members)
        rep_rows.extend(
            BusmapRow(original_bus_uid=u, cluster_bus_uid=rep) for u in members
        )
    logger.info(
        "louvain-mincut: %d buses → %d clusters (target %d, %d anchors)",
        n,
        len(communities),
        target_buses,
        len(anchor_set),
    )
    return sorted(rep_rows, key=lambda r: r.original_bus_uid)


# ---------------------------------------------------------------------------
# Internals
# ---------------------------------------------------------------------------


def _build_weighted_graph(graph: LineGraph, weight_mode: str) -> Any:
    """Undirected bus graph; parallel lines accumulate onto one edge.

    DC lines are excluded: an HVDC link is a controllable device, not a
    coupling — the literature consensus is to never merge buses across an
    HVDC boundary, so it must survive as an inter-cluster corridor.
    """
    import networkx as nx  # noqa: PLC0415

    dc_mask = graph.line_is_dc
    assert dc_mask is not None
    g = nx.Graph()
    g.add_nodes_from(int(u) for u in graph.bus_uids)
    for pos in range(graph.n_lines):
        if dc_mask[pos]:
            continue
        ua = graph.bus_uids[int(graph.line_a[pos])]
        ub = graph.bus_uids[int(graph.line_b[pos])]
        if weight_mode == "capacity":
            w = 0.5 * (float(graph.line_fab[pos]) + float(graph.line_fba[pos]))
        else:
            w = 1.0 / float(graph.line_x[pos])
        if g.has_edge(ua, ub):
            g[ua][ub]["weight"] += w
        else:
            g.add_edge(ua, ub, weight=w)
    # Floor non-positive weights so they still register as connectivity.
    wmax = max((d["weight"] for _, _, d in g.edges(data=True)), default=1.0)
    floor = max(wmax, 1.0) * _WEIGHT_FLOOR_REL
    for _, _, d in g.edges(data=True):
        if d["weight"] <= 0.0:
            d["weight"] = floor
    return g


def _partition_component(sub: Any, comp_k: int, seed: int) -> list[set[int]]:
    """Louvain seed, then merge/split to exactly ``comp_k`` communities."""
    import networkx as nx  # noqa: PLC0415

    nodes = sorted(int(u) for u in sub.nodes)
    if comp_k >= len(nodes):
        return [{u} for u in nodes]
    if comp_k == 1:
        return [set(nodes)]
    comms = [
        set(int(u) for u in c)
        for c in nx.community.louvain_communities(sub, weight="weight", seed=seed)
    ]
    while len(comms) > comp_k:
        comms = _merge_strongest_pair(sub, comms)
    while len(comms) < comp_k:
        split = _split_worst_bottleneck(sub, comms)
        if split is None:
            break
        comms = split
    return comms


def _merge_strongest_pair(sub: Any, comms: list[set[int]]) -> list[set[int]]:
    """Merge the two communities with the largest inter-community weight."""
    label_of = {u: i for i, c in enumerate(comms) for u in c}
    coupling: dict[tuple[int, int], float] = {}
    for ua, ub, d in sub.edges(data=True):
        la, lb = label_of[int(ua)], label_of[int(ub)]
        if la == lb:
            continue
        key = (la, lb) if la < lb else (lb, la)
        coupling[key] = coupling.get(key, 0.0) + float(d["weight"])
    if not coupling:
        # Shouldn't happen inside one connected component; merge smallest two
        # deterministically as a safety net.
        order = sorted(range(len(comms)), key=lambda i: (len(comms[i]), min(comms[i])))
        ia, ib = order[0], order[1]
        merged = comms[ia] | comms[ib]
        return [c for i, c in enumerate(comms) if i not in (ia, ib)] + [merged]
    # Deterministic argmax: break weight ties by smallest member uids.
    (ia, ib), _ = max(
        coupling.items(),
        key=lambda kv: (kv[1], -min(comms[kv[0][0]] | comms[kv[0][1]])),
    )
    merged = comms[ia] | comms[ib]
    return [c for i, c in enumerate(comms) if i not in (ia, ib)] + [merged]


def _split_worst_bottleneck(sub: Any, comms: list[set[int]]) -> list[set[int]] | None:
    """Split the community with the worst internal bottleneck at its min-cut.

    Severity = (corridor weight attached to the community) / (internal
    Stoer–Wagner min-cut).  A community whose internal min-cut is small
    relative to what its corridors can carry hides a transit bottleneck
    that corridor-capacity summing would erase.  Returns ``None`` when no
    community has ≥ 2 nodes.
    """
    import networkx as nx  # noqa: PLC0415

    label_of = {u: i for i, c in enumerate(comms) for u in c}
    attached = [0.0] * len(comms)
    for ua, ub, d in sub.edges(data=True):
        la, lb = label_of[int(ua)], label_of[int(ub)]
        if la != lb:
            attached[la] += float(d["weight"])
            attached[lb] += float(d["weight"])

    best_idx = -1
    best_severity = -1.0
    best_parts: tuple[set[int], set[int]] | None = None
    for i, comm in enumerate(comms):
        if len(comm) < 2:
            continue
        inner = sub.subgraph(comm)
        if not nx.is_connected(inner):
            # Internal connectivity already broken (e.g. only tie was a
            # zero-weight floor edge outside the community): split off the
            # smallest internal component immediately.
            parts = sorted(nx.connected_components(inner), key=len)
            part_a = set(int(u) for u in parts[0])
            part_b = set(u for u in comm) - part_a
            return _apply_split(comms, i, part_a, part_b)
        cut_value, (part_a, part_b) = nx.stoer_wagner(inner, weight="weight")
        severity = (attached[i] + 1.0) / (float(cut_value) + 1e-12)
        # Deterministic tie-break: prefer larger communities, then min uid.
        if severity > best_severity:
            best_severity = severity
            best_idx = i
            best_parts = (set(int(u) for u in part_a), set(int(u) for u in part_b))
    if best_idx < 0 or best_parts is None:
        return None
    return _apply_split(comms, best_idx, best_parts[0], best_parts[1])


def _apply_split(
    comms: list[set[int]], idx: int, part_a: set[int], part_b: set[int]
) -> list[set[int]]:
    out = [c for i, c in enumerate(comms) if i != idx]
    out.append(part_a)
    out.append(part_b)
    return out


def _split_shared_anchor_communities(
    comms: list[set[int]], comp_anchors: list[int]
) -> list[set[int]]:
    """Ensure no two anchors share a community (extras become singletons).

    Mirrors ``_cluster._split_shared_anchor_clusters``: the first anchor
    (smallest uid) keeps the community; every further anchor in the same
    community is split out as a singleton, which may overshoot K.
    """
    if len(comp_anchors) < 2:
        return comms
    out: list[set[int]] = []
    singletons: list[set[int]] = []
    for comm in comms:
        anchors_in = sorted(u for u in comm if u in set(comp_anchors))
        for extra in anchors_in[1:]:
            comm = comm - {extra}
            singletons.append({extra})
        out.append(comm)
    return [c for c in out if c] + singletons
