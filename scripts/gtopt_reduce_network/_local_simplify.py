# SPDX-License-Identifier: BSD-3-Clause
"""Lossless local graph simplifications: parallel-line merge + degree-2
elimination, iterated to a fixed point.

These passes are *exact* for DC-OPF — they do not introduce equivalencing
error. Both passes preserve the original line uids by emitting a
``linemap`` recording every collapse.
"""

from __future__ import annotations

import logging
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Iterable

import numpy as np

from gtopt_reduce_network._busmap import LinemapRow
from gtopt_reduce_network._topology import LineGraph

logger = logging.getLogger(__name__)


@dataclass(slots=True)
class SimplifyResult:
    """Output of :func:`simplify_local`.

    ``surviving_buses`` and ``surviving_line_uids`` describe what remains
    after all merges. ``linemap`` records, for each *original* line uid,
    which surviving line uid carries it (or which deleted bus's pair of
    incident lines absorbed it via series elimination).
    """

    surviving_buses: list[int]
    surviving_line_uids: list[int]
    surviving_a: list[int]  # surviving line endpoints (bus uids)
    surviving_b: list[int]
    surviving_x: list[float]
    surviving_r: list[float]
    surviving_fab: list[float]
    surviving_fba: list[float]
    linemap: list[LinemapRow] = field(default_factory=list)
    eliminated_buses: list[int] = field(default_factory=list)


def simplify_local(
    graph: LineGraph,
    *,
    injection_bus_uids: Iterable[int] = (),
) -> SimplifyResult:
    """Apply parallel-merge + degree-2 elimination until fixed point.

    A bus is eligible for degree-2 elimination only if it appears in
    ``injection_bus_uids`` *false* — i.e. nothing (no generator, demand,
    battery, turbine, hydro junction etc.) injects at the bus. Pass the
    full set of "untouchable" bus uids; everything else is fair game.
    """
    inj_set = set(int(u) for u in injection_bus_uids)
    state = _State.from_graph(graph)

    changed = True
    while changed:
        changed = False
        # Pass 1 — parallel merge.
        merged = _parallel_merge(state)
        if merged:
            changed = True
        # Pass 2 — degree-2 elimination of bare buses.
        elim = _degree2_eliminate(state, untouchable=inj_set)
        if elim:
            changed = True

    return state.to_result()


# ---------------------------------------------------------------------------
# Mutable state ------------------------------------------------------------
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class _Edge:
    uid: int
    a_uid: int  # bus uid of endpoint A
    b_uid: int
    x: float
    r: float
    fab: float
    fba: float
    absorbed: list[int] = field(default_factory=list)  # original uids it now carries


@dataclass(slots=True)
class _State:
    bus_uids: set[int]
    edges: dict[int, _Edge]  # keyed by surviving uid
    next_synth_uid: int  # for series-merged synthetic lines
    incident: dict[int, set[int]] = field(default_factory=dict)
    eliminated_buses: list[int] = field(default_factory=list)
    linemap_rows: list[LinemapRow] = field(default_factory=list)

    @classmethod
    def from_graph(cls, graph: LineGraph) -> _State:
        bus_uids = set(graph.bus_uids)
        edges: dict[int, _Edge] = {}
        max_uid = 0
        for pos, uid in enumerate(graph.line_uids):
            edges[uid] = _Edge(
                uid=uid,
                a_uid=graph.bus_uids[int(graph.line_a[pos])],
                b_uid=graph.bus_uids[int(graph.line_b[pos])],
                x=float(graph.line_x[pos]),
                r=float(graph.line_r[pos]),
                fab=float(graph.line_fab[pos]),
                fba=float(graph.line_fba[pos]),
                absorbed=[uid],
            )
            max_uid = max(max_uid, uid)
        state = cls(
            bus_uids=bus_uids,
            edges=edges,
            next_synth_uid=max_uid + 1_000_001,  # large gap to keep synth uids visible
        )
        state._rebuild_incident()
        return state

    def _rebuild_incident(self) -> None:
        self.incident = {u: set() for u in self.bus_uids}
        for e in self.edges.values():
            self.incident[e.a_uid].add(e.uid)
            self.incident[e.b_uid].add(e.uid)

    def alloc_synth_uid(self) -> int:
        u = self.next_synth_uid
        self.next_synth_uid += 1
        return u

    def to_result(self) -> SimplifyResult:
        surviving = sorted(self.edges)
        return SimplifyResult(
            surviving_buses=sorted(self.bus_uids),
            surviving_line_uids=surviving,
            surviving_a=[self.edges[u].a_uid for u in surviving],
            surviving_b=[self.edges[u].b_uid for u in surviving],
            surviving_x=[self.edges[u].x for u in surviving],
            surviving_r=[self.edges[u].r for u in surviving],
            surviving_fab=[self.edges[u].fab for u in surviving],
            surviving_fba=[self.edges[u].fba for u in surviving],
            linemap=self.linemap_rows,
            eliminated_buses=self.eliminated_buses,
        )


# ---------------------------------------------------------------------------
# Passes -------------------------------------------------------------------
# ---------------------------------------------------------------------------


def _parallel_merge(state: _State) -> int:
    groups: dict[tuple[int, int], list[int]] = defaultdict(list)
    for e in state.edges.values():
        a, b = (e.a_uid, e.b_uid) if e.a_uid < e.b_uid else (e.b_uid, e.a_uid)
        groups[a, b].append(e.uid)

    merged = 0
    for (a, b), uids in groups.items():
        if len(uids) <= 1:
            continue
        edges = [state.edges[u] for u in uids]
        x_eq = 1.0 / np.sum([1.0 / e.x for e in edges])
        # Loss energy at full capacity: R_eq = X_eq^2 * Σ R_i / X_i^2.
        r_eq = (x_eq**2) * float(np.sum([e.r / (e.x**2) for e in edges]))
        # Capacity sum (worst case for parallel): tmax_ab respects original
        # direction convention since (a,b) is fixed for the group.
        fab_eq = float(np.sum([_dir_cap(e, a, b, ab=True) for e in edges]))
        fba_eq = float(np.sum([_dir_cap(e, a, b, ab=False) for e in edges]))
        new_uid = state.alloc_synth_uid()
        absorbed: list[int] = []
        for e in edges:
            absorbed.extend(e.absorbed)
        state.edges[new_uid] = _Edge(
            uid=new_uid,
            a_uid=a,
            b_uid=b,
            x=float(x_eq),
            r=r_eq,
            fab=fab_eq,
            fba=fba_eq,
            absorbed=absorbed,
        )
        for u in uids:
            del state.edges[u]
        for orig in absorbed:
            state.linemap_rows.append(
                LinemapRow(
                    original_line_uid=orig,
                    equivalent_line_uid=new_uid,
                    rule="parallel-merge",
                )
            )
        merged += 1

    if merged:
        state._rebuild_incident()
        logger.debug("parallel_merge: %d corridors merged", merged)
    return merged


def _degree2_eliminate(state: _State, *, untouchable: set[int]) -> int:
    eliminated = 0
    # Snapshot to allow mutation while iterating.
    candidates = [u for u in list(state.bus_uids) if u not in untouchable]
    for bus in candidates:
        if bus not in state.bus_uids:
            continue  # already removed during this pass
        incident = state.incident.get(bus, set())
        if len(incident) != 2:
            continue
        e1_uid, e2_uid = sorted(incident)
        e1 = state.edges[e1_uid]
        e2 = state.edges[e2_uid]
        # Identify the two "outer" endpoints != bus.
        other1 = e1.b_uid if e1.a_uid == bus else e1.a_uid
        other2 = e2.b_uid if e2.a_uid == bus else e2.a_uid
        if other1 == other2:
            # Self-loop after elimination — leave it; parallel pass will deal.
            continue
        # Series merge X_eq = X1 + X2, R_eq = R1 + R2, F_eq = min directional caps.
        x_eq = e1.x + e2.x
        r_eq = e1.r + e2.r
        fab_eq, fba_eq = _series_caps(e1, e2, bus, other1, other2)
        new_uid = state.alloc_synth_uid()
        state.edges[new_uid] = _Edge(
            uid=new_uid,
            a_uid=other1,
            b_uid=other2,
            x=x_eq,
            r=r_eq,
            fab=fab_eq,
            fba=fba_eq,
            absorbed=e1.absorbed + e2.absorbed,
        )
        for orig in e1.absorbed + e2.absorbed:
            state.linemap_rows.append(
                LinemapRow(
                    original_line_uid=orig,
                    equivalent_line_uid=new_uid,
                    rule="series-merge",
                )
            )
        del state.edges[e1_uid]
        del state.edges[e2_uid]
        state.bus_uids.discard(bus)
        state.eliminated_buses.append(bus)
        state._rebuild_incident()
        eliminated += 1

    if eliminated:
        logger.debug("degree2_eliminate: %d buses removed", eliminated)
    return eliminated


# ---------------------------------------------------------------------------
# Helpers ------------------------------------------------------------------
# ---------------------------------------------------------------------------


def _dir_cap(edge: _Edge, group_a: int, group_b: int, *, ab: bool) -> float:
    """Capacity in the (group_a -> group_b) direction (or reverse) for ``edge``."""
    forward = edge.a_uid == group_a and edge.b_uid == group_b
    if ab:
        return edge.fab if forward else edge.fba
    return edge.fba if forward else edge.fab


def _series_caps(
    e1: _Edge, e2: _Edge, mid: int, other1: int, other2: int
) -> tuple[float, float]:
    """Series-combined directional capacities ``(other1 → other2, reverse)``."""
    # Direction other1 → mid → other2 uses e1.dir(other1→mid) and e2.dir(mid→other2).
    e1_o1_mid = e1.fab if e1.a_uid == other1 else e1.fba
    e2_mid_o2 = e2.fab if e2.a_uid == mid else e2.fba
    fab = min(e1_o1_mid, e2_mid_o2)
    e2_o2_mid = e2.fab if e2.a_uid == other2 else e2.fba
    e1_mid_o1 = e1.fab if e1.a_uid == mid else e1.fba
    fba = min(e2_o2_mid, e1_mid_o1)
    return fab, fba
