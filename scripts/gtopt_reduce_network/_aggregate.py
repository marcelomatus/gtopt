# SPDX-License-Identifier: BSD-3-Clause
"""Aggregate inter-cluster lines after the busmap is fixed.

For each cluster pair ``(u, v)`` enumerated in the original line set we

* sum directional capacities ``F_uv = Σ F_ℓ``
* combine reactances by series-parallel rule (default) or PTDF-fit
* combine resistances to preserve loss energy at full load
* drop intra-cluster lines and record them in the linemap

Returns the surviving line list ready to be inserted into the reduced
case JSON.
"""

from __future__ import annotations

import logging
from collections import defaultdict
from dataclasses import dataclass, field

import numpy as np

from gtopt_reduce_network._busmap import LinemapRow

logger = logging.getLogger(__name__)


@dataclass(slots=True)
class AggregatedLine:
    """A single equivalent line emitted by aggregation.

    ``reactance`` is ``None`` on DC equivalents (``is_dc=True``): gtopt
    models an HVDC line as a line without reactance, whose KVL row is
    skipped and only the capacity bounds apply.
    """

    uid: int
    name: str
    bus_a_uid: int
    bus_b_uid: int
    reactance: float | None
    resistance: float
    tmax_ab: float
    tmax_ba: float
    absorbed: list[int] = field(default_factory=list)
    is_dc: bool = False


def aggregate_lines(
    surviving_line_uids: list[int],
    surviving_a: list[int],
    surviving_b: list[int],
    surviving_x: list[float],
    surviving_r: list[float],
    surviving_fab: list[float],
    surviving_fba: list[float],
    cluster_of_bus: dict[int, int],
    *,
    rule: str = "series-parallel",
    next_line_uid: int | None = None,
    surviving_is_dc: list[bool] | None = None,
) -> tuple[list[AggregatedLine], list[LinemapRow]]:
    """Build the equivalent inter-cluster line list.

    The ``surviving_*`` arrays come from :class:`_local_simplify.SimplifyResult`
    or directly from the original :class:`LineGraph`. Lines whose two
    endpoints land in the same cluster become *intra-cluster* and are
    recorded in the linemap with ``rule="intra-cluster"``.

    AC and DC members of a corridor are aggregated separately: the AC
    members combine reactance in parallel; the DC members (controllable
    HVDC, no X) emit a distinct DC equivalent so the reduced case keeps
    the controllable corridor explicit.
    """
    if rule not in ("series-parallel",):
        raise ValueError(f"unknown reactance rule {rule!r}; supported: series-parallel")
    dc_flags = (
        surviving_is_dc
        if surviving_is_dc is not None
        else [False] * len(surviving_line_uids)
    )
    groups: dict[tuple[int, int], list[int]] = defaultdict(list)
    intra: list[LinemapRow] = []
    for pos, uid in enumerate(surviving_line_uids):
        a_clu = cluster_of_bus[surviving_a[pos]]
        b_clu = cluster_of_bus[surviving_b[pos]]
        if a_clu == b_clu:
            intra.append(
                LinemapRow(
                    original_line_uid=uid,
                    equivalent_line_uid=None,
                    rule="intra-cluster",
                )
            )
            continue
        a, b = (a_clu, b_clu) if a_clu < b_clu else (b_clu, a_clu)
        groups[a, b].append(pos)

    aggregated: list[AggregatedLine] = []
    line_rows: list[LinemapRow] = []
    next_uid = (
        next_line_uid
        if next_line_uid is not None
        else (max(surviving_line_uids) + 1 if surviving_line_uids else 1)
    )

    for (a_clu, b_clu), positions in sorted(groups.items()):
        positions = sorted(positions)
        ac_pos = [p for p in positions if not dc_flags[p]]
        dc_pos = [p for p in positions if dc_flags[p]]
        for member_pos, is_dc in ((ac_pos, False), (dc_pos, True)):
            if not member_pos:
                continue
            new_uid = next_uid
            next_uid += 1
            aggregated.append(
                _aggregate_corridor(
                    member_pos,
                    a_clu,
                    b_clu,
                    new_uid,
                    is_dc,
                    surviving_line_uids,
                    surviving_a,
                    surviving_x,
                    surviving_r,
                    surviving_fab,
                    surviving_fba,
                    cluster_of_bus,
                )
            )
            for orig in aggregated[-1].absorbed:
                line_rows.append(
                    LinemapRow(
                        original_line_uid=orig,
                        equivalent_line_uid=new_uid,
                        rule="inter-cluster",
                    )
                )
    return aggregated, line_rows + intra


def _aggregate_corridor(
    positions: list[int],
    a_clu: int,
    b_clu: int,
    new_uid: int,
    is_dc: bool,
    surviving_line_uids: list[int],
    surviving_a: list[int],
    surviving_x: list[float],
    surviving_r: list[float],
    surviving_fab: list[float],
    surviving_fba: list[float],
    cluster_of_bus: dict[int, int],
) -> AggregatedLine:
    """Equivalent line for one corridor's AC (or DC) member set."""
    if is_dc:
        # Controllable HVDC members: no reactance; resistances (when all
        # present) combine as plain parallel resistors.
        x_eq: float | None = None
        rs = [surviving_r[p] for p in positions]
        r_eq = (
            1.0 / float(np.sum([1.0 / r for r in rs]))
            if all(r > 0 for r in rs)
            else 0.0
        )
    else:
        # Reactances combine by parallel rule across all chosen positions
        # (series-parallel reduction across a multi-line corridor between
        # two clusters degenerates to parallel-only since the two endpoints
        # are the boundary nodes); R_eq preserves loss energy at full load.
        x_eq = 1.0 / float(np.sum([1.0 / surviving_x[p] for p in positions]))
        r_eq = (x_eq**2) * float(
            np.sum([surviving_r[p] / (surviving_x[p] ** 2) for p in positions])
        )
    # Capacities sum, but respect direction: each original line has its
    # A→B / B→A meaning; map to the cluster ordering.
    fab_sum = 0.0
    fba_sum = 0.0
    for p in positions:
        forward = cluster_of_bus[surviving_a[p]] == a_clu
        if forward:
            fab_sum += surviving_fab[p]
            fba_sum += surviving_fba[p]
        else:
            fab_sum += surviving_fba[p]
            fba_sum += surviving_fab[p]
    return AggregatedLine(
        uid=new_uid,
        name=f"agg{'dc' if is_dc else ''}_{a_clu}_{b_clu}",
        bus_a_uid=a_clu,
        bus_b_uid=b_clu,
        reactance=x_eq,
        resistance=r_eq,
        tmax_ab=fab_sum,
        tmax_ba=fba_sum,
        absorbed=[surviving_line_uids[p] for p in positions],
        is_dc=is_dc,
    )
