# SPDX-License-Identifier: BSD-3-Clause
"""Anchor-constrained hierarchical clustering of buses.

Workflow:

1. ``select_anchors`` returns a set of bus uids that must each survive
   in their own cluster (top-N by injection magnitude, plus user list,
   plus optional reservoir hosts).
2. ``build_busmap`` runs ``scipy.cluster.hierarchy.linkage(method="average")``
   on the condensed distance matrix between non-anchor buses, then
   assigns each non-anchor bus to its nearest anchor cluster (hard
   constraint), filling residual clusters via the HAC dendrogram cut.

The output is a :class:`BusmapRow` list: ``original_bus_uid`` →
``cluster_bus_uid``. Cluster ids are themselves bus uids — chosen as
the cluster's anchor (if any) or the cluster member with the smallest
uid (deterministic tie-break).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any, Iterable

import numpy as np
from scipy.cluster.hierarchy import fcluster, linkage
from scipy.spatial.distance import squareform

from gtopt_reduce_network._busmap import BusmapRow
from gtopt_reduce_network._io import Case, resolve_bus_ref

logger = logging.getLogger(__name__)


@dataclass(slots=True)
class AnchorSelection:
    """Selected anchor bus uids and the rules that picked each."""

    bus_uids: list[int]
    by_rule: dict[int, list[str]]


def select_anchors(
    case: Case,
    *,
    target_buses: int,
    surviving_bus_uids: Iterable[int],
    user_anchor_uids: Iterable[int] = (),
    min_load_mw: float | None = None,
    min_gen_capacity_mw: float | None = None,
    include_reservoir_hosts: bool = True,
    top_n_by_injection: int | None = None,
) -> AnchorSelection:
    """Pick anchor bus uids by union of rules.

    The default ``top_n_by_injection`` is ``max(target_buses // 4, 8)``
    capped at ``len(surviving_bus_uids)``. Caller can override.
    """
    surviving = sorted(int(u) for u in surviving_bus_uids)
    surviving_set = set(surviving)
    by_rule: dict[int, list[str]] = {}

    def _add(uid: int, rule: str) -> None:
        if uid not in surviving_set:
            return
        by_rule.setdefault(uid, []).append(rule)

    # User-supplied list.
    for uid in user_anchor_uids:
        _add(int(uid), "user-list")

    # Top-N by injection magnitude. Cap at target_buses so anchors never
    # exceed K (which would make the clustering infeasible).
    if top_n_by_injection is None:
        top_n_by_injection = max(target_buses // 4, 1)
        top_n_by_injection = min(top_n_by_injection, target_buses, len(surviving))
    inj_mag = _injection_magnitudes(case, surviving)
    top_idx = np.argsort(-np.asarray([inj_mag.get(u, 0.0) for u in surviving]))
    for k in top_idx[:top_n_by_injection]:
        if inj_mag.get(surviving[int(k)], 0.0) > 0.0:
            _add(surviving[int(k)], "top-injection")

    # Load / generation thresholds.
    if min_load_mw is not None:
        load_by_bus = _peak_load_by_bus(case)
        for uid, load in load_by_bus.items():
            if load >= min_load_mw:
                _add(uid, "min-load")
    if min_gen_capacity_mw is not None:
        gen_by_bus = _gen_capacity_by_bus(case)
        for uid, cap in gen_by_bus.items():
            if cap >= min_gen_capacity_mw:
                _add(uid, "min-gen")

    # Reservoir hosts.  The C++ Turbine schema has no ``bus`` field — the
    # host bus is reached through the turbine's ``generator`` reference
    # (turbine.hpp: waterway/flow/junctions/generator).  Accept a direct
    # ``bus`` field anyway for hand-written cases.
    if include_reservoir_hosts:
        gen_bus_by_uid, gen_bus_by_name = _generator_bus_maps(case)
        for turb in case.array("turbine_array"):
            bus_uid: int | None = None
            try:
                bus_uid = resolve_bus_ref(case, turb.get("bus"))
            except (KeyError, TypeError):
                bus_uid = None
            if bus_uid is None:
                bus_uid = _turbine_host_bus(turb, gen_bus_by_uid, gen_bus_by_name)
            if bus_uid is not None:
                _add(bus_uid, "reservoir-host")

    # Never exceed K anchors (build_busmap requires anchors ≤ K).  Keep the
    # user list untouched; trim the rule-derived rest by injection size.
    anchors = sorted(by_rule)
    if len(anchors) > target_buses:
        user = [u for u in anchors if "user-list" in by_rule[u]]
        if len(user) > target_buses:
            raise ValueError(
                f"{len(user)} user anchors exceed target_buses={target_buses}"
            )
        rest = [u for u in anchors if "user-list" not in by_rule[u]]
        inj = _injection_magnitudes(case, rest)
        rest.sort(key=lambda u: (-inj.get(u, 0.0), u))
        keep = set(user) | set(rest[: target_buses - len(user)])
        logger.warning(
            "anchor count %d exceeds target K=%d; keeping the %d largest by injection",
            len(anchors),
            target_buses,
            len(keep),
        )
        by_rule = {u: rules for u, rules in by_rule.items() if u in keep}

    return AnchorSelection(bus_uids=sorted(by_rule), by_rule=by_rule)


def _generator_bus_maps(case: Case) -> tuple[dict[int, int], dict[str, int]]:
    """Map generator uid/name → host bus uid (skips unresolvable refs)."""
    by_uid: dict[int, int] = {}
    by_name: dict[str, int] = {}
    for gen in case.array("generator_array"):
        try:
            bus_uid = resolve_bus_ref(case, gen.get("bus"))
        except (KeyError, TypeError):
            continue
        if bus_uid is None:
            continue
        if "uid" in gen:
            by_uid[int(gen["uid"])] = bus_uid
        if "name" in gen:
            by_name[str(gen["name"])] = bus_uid
    return by_uid, by_name


def _turbine_host_bus(
    turb: dict[str, Any], by_uid: dict[int, int], by_name: dict[str, int]
) -> int | None:
    """Resolve a turbine's host bus through its ``generator`` reference."""
    ref = turb.get("generator")
    if ref is None or isinstance(ref, bool):
        return None
    if isinstance(ref, int):
        return by_uid.get(ref)
    if isinstance(ref, str):
        return by_name.get(ref)
    return None


def build_busmap(
    bus_uids: list[int],
    distance: np.ndarray,
    *,
    target_buses: int,
    anchors: Iterable[int],
) -> list[BusmapRow]:
    """Cluster ``bus_uids`` into ``target_buses`` groups respecting anchors.

    ``distance`` is a square matrix indexed by the order of ``bus_uids``.
    Disconnected pairs (``np.inf``) are clustered separately by component.
    """
    n = len(bus_uids)
    if n == 0:
        return []
    bus_index = {u: i for i, u in enumerate(bus_uids)}
    anchor_set = sorted(set(int(a) for a in anchors) & set(bus_uids))
    if target_buses < max(1, len(anchor_set)):
        raise ValueError(
            f"target_buses={target_buses} is below anchor count "
            f"{len(anchor_set)}; pick a larger K or fewer anchors"
        )
    if target_buses >= n:
        # Trivial: every bus survives.
        return [BusmapRow(original_bus_uid=u, cluster_bus_uid=u) for u in bus_uids]

    cluster_id_of: dict[int, int] = {}

    # Split into components by inf-cuts in ``distance``.
    components = _components_from_distance(distance)
    cluster_uid_counter = 0

    for comp in components:
        comp_uids = [bus_uids[i] for i in comp]
        comp_anchors = [u for u in comp_uids if u in anchor_set]
        # Per-component target K: proportional to size, with at least
        # max(len(anchors), 1).
        comp_K = max(
            len(comp_anchors),
            int(round(target_buses * (len(comp) / n))),
            1,
        )
        comp_K = min(comp_K, len(comp))
        labels = _cluster_component(distance, comp, comp_anchors, comp_K, bus_index)
        for local_idx, lbl in zip(comp, labels):
            cluster_id_of[bus_uids[local_idx]] = cluster_uid_counter + lbl
        cluster_uid_counter += int(np.max(labels)) + 1

    # Map cluster id → representative bus uid (anchor preferred, else min uid).
    cluster_members: dict[int, list[int]] = {}
    for bus_uid, cid in cluster_id_of.items():
        cluster_members.setdefault(cid, []).append(bus_uid)
    rep_of: dict[int, int] = {}
    for cid, members in cluster_members.items():
        anchors_in = [u for u in members if u in anchor_set]
        rep_of[cid] = min(anchors_in) if anchors_in else min(members)

    return [
        BusmapRow(original_bus_uid=u, cluster_bus_uid=rep_of[cid])
        for u, cid in sorted(cluster_id_of.items())
    ]


# ---------------------------------------------------------------------------
# Internals ----------------------------------------------------------------
# ---------------------------------------------------------------------------


def _components_from_distance(distance: np.ndarray) -> list[list[int]]:
    n = distance.shape[0]
    seen = [False] * n
    out: list[list[int]] = []
    for start in range(n):
        if seen[start]:
            continue
        comp: list[int] = []
        stack = [start]
        while stack:
            i = stack.pop()
            if seen[i]:
                continue
            seen[i] = True
            comp.append(i)
            for j in range(n):
                if not seen[j] and np.isfinite(distance[i, j]):
                    stack.append(j)
        out.append(sorted(comp))
    return out


def _cluster_component(
    distance: np.ndarray,
    comp: list[int],
    comp_anchors: list[int],
    comp_K: int,
    bus_index: dict[int, int],
) -> np.ndarray:
    """Return cluster labels (0..comp_K-1) for the buses in ``comp``."""
    if len(comp) == 1 or comp_K == 1:
        return np.zeros(len(comp), dtype=int)
    sub = distance[np.ix_(comp, comp)].copy()
    # Replace inf with a large finite stand-in so squareform doesn't choke
    # (we already split by component, so any remaining inf is a numerical
    # artefact).
    finite_max = float(np.nanmax(sub[np.isfinite(sub)]))
    sub[~np.isfinite(sub)] = finite_max * 1e3 + 1.0
    np.fill_diagonal(sub, 0.0)
    # Make symmetric (dijkstra returns symmetric for undirected; defensive).
    sub = 0.5 * (sub + sub.T)
    condensed = squareform(sub, checks=False)
    z = linkage(condensed, method="average")
    labels = fcluster(z, t=comp_K, criterion="maxclust").astype(int) - 1

    # Anchor-split: ensure no two anchors share a label.
    if len(comp_anchors) > 1:
        pos_in_comp = {global_i: pi for pi, global_i in enumerate(comp)}
        anchor_positions = [pos_in_comp[bus_index[a]] for a in comp_anchors]
        labels = _split_shared_anchor_clusters(labels, anchor_positions)
    return labels


def _split_shared_anchor_clusters(
    labels: np.ndarray, anchor_positions: list[int]
) -> np.ndarray:
    """Relabel so each anchor sits in a distinct cluster.

    Uses largest-uid-wins re-labeling: walk anchors in order, and any
    anchor sharing a label with an earlier anchor gets a fresh label.
    """
    out = labels.copy()
    seen_label_for_anchor: dict[int, int] = {}
    next_label = int(out.max()) + 1
    for pos in sorted(anchor_positions):
        lbl = int(out[pos])
        if lbl in seen_label_for_anchor.values():
            out[pos] = next_label
            seen_label_for_anchor[pos] = next_label
            next_label += 1
        else:
            seen_label_for_anchor[pos] = lbl
    return out


# ---------------------------------------------------------------------------
# Injection helpers --------------------------------------------------------
# ---------------------------------------------------------------------------


def _injection_magnitudes(case: Case, bus_uids: list[int]) -> dict[int, float]:
    """Approximate injection magnitude per bus = Σ gen capacity + Σ peak load."""
    gens = _gen_capacity_by_bus(case)
    loads = _peak_load_by_bus(case)
    out: dict[int, float] = {}
    for u in bus_uids:
        out[u] = gens.get(u, 0.0) + loads.get(u, 0.0)
    return out


def _peak_load_by_bus(case: Case) -> dict[int, float]:
    out: dict[int, float] = {}
    for d in case.array("demand_array"):
        try:
            bus_uid = resolve_bus_ref(case, d.get("bus"))
        except (KeyError, TypeError):
            continue
        if bus_uid is None:
            continue
        peak = _scalar_peak(d.get("lmax"))
        out[bus_uid] = out.get(bus_uid, 0.0) + peak
    return out


def _gen_capacity_by_bus(case: Case) -> dict[int, float]:
    out: dict[int, float] = {}
    for g in case.array("generator_array"):
        try:
            bus_uid = resolve_bus_ref(case, g.get("bus"))
        except (KeyError, TypeError):
            continue
        if bus_uid is None:
            continue
        cap = _scalar_peak(g.get("capacity"), fallback=g.get("pmax"))
        out[bus_uid] = out.get(bus_uid, 0.0) + cap
    return out


def _scalar_peak(value: object, fallback: object = None) -> float:
    """Reduce a scalar/array/string field to a representative magnitude."""
    if isinstance(value, bool):
        value = None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, list):
        flat: list[float] = []
        stack: list[object] = list(value)
        while stack:
            x = stack.pop()
            if isinstance(x, list):
                stack.extend(x)
            elif isinstance(x, (int, float)) and not isinstance(x, bool):
                flat.append(float(x))
        if flat:
            return max(flat)
    if fallback is not None:
        return _scalar_peak(fallback)
    return 0.0
