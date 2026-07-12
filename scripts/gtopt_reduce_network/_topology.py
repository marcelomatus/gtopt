# SPDX-License-Identifier: BSD-3-Clause
"""Topology helpers: admittance matrix, line graph, parallel-line dedup.

Operates on numpy arrays / dicts derived from a Case to keep the topology
math independent of the JSON serialisation.

DC lines: gtopt models an HVDC line as a line without reactance (or
X = 0) — the KVL emitter skips it (``kirchhoff_node_angle.cpp``) and only
its capacity bounds apply — optionally tagged ``type: "dc"``.  The line
graph mirrors that: such lines are KEPT, flagged in ``line_is_dc``, carry
``line_x = inf`` internally (so ``1/x`` contributes 0 susceptance), and
are excluded from every reactance-based computation while keeping their
transfer capability for clustering and corridor aggregation.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Any

import numpy as np
from scipy.sparse import csr_matrix

from gtopt_reduce_network._io import Case, resolve_bus_ref

logger = logging.getLogger(__name__)


@dataclass(slots=True)
class LineGraph:
    """Sparse representation of the line graph derived from a Case.

    All buses appear in ``bus_uids`` even if they have no incident line.
    DC lines (``type: "dc"`` or missing/non-positive reactance) are kept
    with ``line_is_dc`` set and ``line_x = inf``; lines with unresolvable
    bus endpoints are *excluded* and reported via ``skipped_line_uids``.
    """

    bus_uids: list[int]
    bus_index: dict[int, int]
    line_uids: list[int]
    line_a: np.ndarray  # shape (n_lines,), local bus index of endpoint A
    line_b: np.ndarray
    line_x: np.ndarray  # reactance (p.u. or Ω); inf on DC lines
    line_r: np.ndarray  # resistance, 0 when absent
    line_fab: np.ndarray  # tmax_ab (MW)
    line_fba: np.ndarray  # tmax_ba (MW)
    skipped_line_uids: list[int] = field(default_factory=list)
    line_is_dc: np.ndarray | None = None  # bool mask; None → all AC

    def __post_init__(self) -> None:
        if self.line_is_dc is None:
            self.line_is_dc = np.zeros(len(self.line_uids), dtype=bool)

    @property
    def n_buses(self) -> int:
        return len(self.bus_uids)

    @property
    def n_lines(self) -> int:
        return len(self.line_uids)

    @property
    def dc_line_uids(self) -> list[int]:
        mask = self.line_is_dc
        assert mask is not None
        return [u for u, dc in zip(self.line_uids, mask) if dc]


def build_line_graph(case: Case) -> LineGraph:
    """Extract the line-graph view from a Case.

    A line is classified DC when it carries ``type: "dc"`` or when its
    ``reactance`` is missing, non-positive, or non-scalar — mirroring
    gtopt's own KVL emitter, which skips exactly those lines
    (``kirchhoff_node_angle.cpp``).  DC lines keep their capacity for
    clustering/aggregation but contribute no susceptance.
    """
    bus_uids = sorted(case.bus_name_by_uid)
    bus_index = {u: i for i, u in enumerate(bus_uids)}

    line_uids: list[int] = []
    a_idx: list[int] = []
    b_idx: list[int] = []
    xs: list[float] = []
    rs: list[float] = []
    fabs: list[float] = []
    fbas: list[float] = []
    dcs: list[bool] = []
    skipped: list[int] = []

    for line in case.array("line_array"):
        if not _line_active(line):
            continue
        uid = int(line["uid"])
        x = _scalar_or_none(line.get("reactance"))
        is_dc = _line_type_is_dc(line) or x is None or x <= 0.0
        try:
            ua = resolve_bus_ref(case, line.get("bus_a"))
            ub = resolve_bus_ref(case, line.get("bus_b"))
        except (KeyError, TypeError) as exc:
            logger.warning("skip line uid=%s: %s", uid, exc)
            skipped.append(uid)
            continue
        if ua is None or ub is None or ua == ub:
            skipped.append(uid)
            continue
        line_uids.append(uid)
        a_idx.append(bus_index[ua])
        b_idx.append(bus_index[ub])
        xs.append(np.inf if is_dc else float(x))  # type: ignore[arg-type]
        rs.append(float(_scalar_or_none(line.get("resistance")) or 0.0))
        fabs.append(float(_scalar_or_none(line.get("tmax_ab")) or 0.0))
        fbas.append(float(_scalar_or_none(line.get("tmax_ba")) or 0.0))
        dcs.append(is_dc)

    n_dc = sum(dcs)
    if n_dc:
        logger.info(
            "modelled %d DC lines (type 'dc' or no/zero reactance): uids %s",
            n_dc,
            [u for u, dc in zip(line_uids, dcs) if dc][:20],
        )

    return LineGraph(
        bus_uids=bus_uids,
        bus_index=bus_index,
        line_uids=line_uids,
        line_a=np.asarray(a_idx, dtype=np.int64),
        line_b=np.asarray(b_idx, dtype=np.int64),
        line_x=np.asarray(xs, dtype=float),
        line_r=np.asarray(rs, dtype=float),
        line_fab=np.asarray(fabs, dtype=float),
        line_fba=np.asarray(fbas, dtype=float),
        skipped_line_uids=skipped,
        line_is_dc=np.asarray(dcs, dtype=bool),
    )


def _line_type_is_dc(line: dict[str, Any]) -> bool:
    """True when the line's optional ``type`` tag marks it as DC."""
    t = line.get("type")
    return isinstance(t, str) and t.strip().lower() == "dc"


def build_admittance(graph: LineGraph) -> csr_matrix:
    """Sparse DC bus-susceptance matrix B = A diag(1/x) Aᵀ.

    Sign convention: B is positive semidefinite; the diagonal carries
    Σ(1/x) over incident lines, off-diagonal carries -Σ(1/x) over
    parallel lines connecting the bus pair.
    """
    n = graph.n_buses
    inv_x = 1.0 / graph.line_x
    rows = np.concatenate([graph.line_a, graph.line_b, graph.line_a, graph.line_b])
    cols = np.concatenate([graph.line_a, graph.line_b, graph.line_b, graph.line_a])
    data = np.concatenate([inv_x, inv_x, -inv_x, -inv_x])
    return csr_matrix((data, (rows, cols)), shape=(n, n))


def build_undirected_adjacency(graph: LineGraph) -> csr_matrix:
    """Sparse undirected adjacency weighted by ``|x|`` (for shortest-path).

    Parallel lines are aggregated using the parallel-reactance rule
    ``X_par = (Σ 1/X)⁻¹`` so dijkstra sees the equivalent edge weight.
    """
    n = graph.n_buses
    if graph.n_lines == 0:
        return csr_matrix((n, n))
    pair_inv: dict[tuple[int, int], float] = {}
    for a, b, x in zip(graph.line_a, graph.line_b, graph.line_x):
        a_i, b_i = int(a), int(b)
        if a_i > b_i:
            a_i, b_i = b_i, a_i
        pair_inv[a_i, b_i] = pair_inv.get((a_i, b_i), 0.0) + 1.0 / float(x)

    rows: list[int] = []
    cols: list[int] = []
    data: list[float] = []
    for (a_i, b_i), inv_x in pair_inv.items():
        if inv_x <= 0.0:
            continue  # pure-DC pair: no susceptance coupling
        x_eq = 1.0 / inv_x
        rows.extend((a_i, b_i))
        cols.extend((b_i, a_i))
        data.extend((x_eq, x_eq))
    return csr_matrix((data, (rows, cols)), shape=(n, n))


def find_parallel_groups(graph: LineGraph) -> dict[tuple[int, int], list[int]]:
    """Group line *positions* (not uids) by their unordered (a, b) endpoints."""
    groups: dict[tuple[int, int], list[int]] = {}
    for pos, (a, b) in enumerate(zip(graph.line_a, graph.line_b)):
        a_i, b_i = int(a), int(b)
        if a_i == b_i:
            continue
        if a_i > b_i:
            a_i, b_i = b_i, a_i
        groups.setdefault((a_i, b_i), []).append(pos)
    return groups


def _line_active(line: dict[str, Any]) -> bool:
    if "active" not in line:
        return True
    return bool(line["active"])


def _scalar_or_none(value: Any) -> float | None:
    if value is None:
        return None
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    return None
