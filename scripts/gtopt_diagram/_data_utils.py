# SPDX-License-Identifier: BSD-3-Clause
"""Voltage-level bus reduction utilities for gtopt diagrams.

Provides functions for reducing large power system diagrams by aggregating
low-voltage buses into their nearest high-voltage neighbours:

- :func:`build_voltage_map` -- BFS-based voltage reduction mapping.
- :func:`count_visible_buses` -- count representative buses after reduction.
- :func:`auto_voltage_threshold` -- automatically choose a voltage threshold.
- :func:`resolve_bus_ref` -- translate a bus reference through the voltage map.
"""

from __future__ import annotations

from collections import defaultdict
from typing import Optional

from ._graph_model import AUTO_MAX_HV_BUSES

# Maximum BFS depth when searching for the nearest HV bus; prevents infinite
# loops in pathological cases while being sufficient for any real power network
VOLTAGE_BFS_MAX_DEPTH = 20


# ---------------------------------------------------------------------------
# Voltage-level bus reduction
# ---------------------------------------------------------------------------


def build_voltage_map(
    buses: list[dict],
    lines: list[dict],
    threshold: float,
) -> dict:
    """Return a mapping {bus_ref -> representative_hv_bus_ref}.

    Every bus whose ``voltage`` field is set and is **strictly less** than
    *threshold* [kV] is mapped to the nearest bus with voltage >= threshold
    reachable via the line network.  Buses without a voltage field and buses
    already at or above the threshold map to themselves.

    The representative bus is chosen by BFS over the adjacency graph built
    from ``line_array``.  If no high-voltage neighbour is found within
    :data:`VOLTAGE_BFS_MAX_DEPTH` hops the low-voltage bus is left unmapped
    (maps to itself).
    """
    if threshold <= 0:
        return {}

    # Build adjacency using whichever reference type appears in the lines
    adj: dict = defaultdict(set)
    for line in lines:
        a = line.get("bus_a")
        b = line.get("bus_b")
        if a is not None and b is not None:
            adj[a].add(b)
            adj[b].add(a)

    # Collect voltage per bus reference (uid and name both used as keys)
    bus_voltage: dict = {}
    for bus in buses:
        uid = bus.get("uid")
        name = bus.get("name")
        v = bus.get("voltage")
        try:
            fv: Optional[float] = float(v) if v is not None else None
        except (ValueError, TypeError):
            fv = None
        for ref in (uid, name):
            if ref is not None:
                bus_voltage[ref] = fv

    def _is_hv(ref) -> bool:  # noqa: ANN001
        v = bus_voltage.get(ref)
        # unknown voltage -> treat as HV to avoid unintended lumping of
        # buses whose voltage was not provided in the JSON
        return v is None or v >= threshold

    result: dict = {}
    for bus in buses:
        uid = bus.get("uid")
        name = bus.get("name")
        ref = uid if uid is not None else name
        if ref is None:
            continue
        if _is_hv(ref):
            result[ref] = ref
            if name is not None and name != ref:
                result[name] = name
            continue

        # BFS from this LV bus to find the nearest HV bus
        visited: set = {uid, name} - {None}
        queue = list(visited)
        found = None
        for _ in range(VOLTAGE_BFS_MAX_DEPTH):
            if not queue:
                break
            next_q: list = []
            for curr in queue:
                for nb in adj.get(curr, set()):
                    if nb in visited:
                        continue
                    if _is_hv(nb):
                        found = nb
                        break
                    visited.add(nb)
                    next_q.append(nb)
                if found:
                    break
            if found:
                break
            queue = next_q

        rep = found if found is not None else ref
        result[ref] = rep
        if name is not None and name != ref:
            result[name] = rep

    return result


def count_visible_buses(buses: list[dict], lines: list[dict], threshold: float) -> int:
    """Return the number of distinct representative buses after voltage reduction.

    A bus is a representative if at least one other bus (including itself) maps
    to it after the BFS voltage reduction.  This is the number of *visible* bus
    nodes that would appear in the diagram at the given threshold.
    """
    if threshold <= 0:
        return len(buses)
    vmap = build_voltage_map(buses, lines, threshold)
    representatives: set[int | str] = set()
    for bus in buses:
        uid = bus.get("uid")
        name = bus.get("name")
        ref = uid if uid is not None else name
        if ref is None:
            continue
        representatives.add(vmap.get(ref, ref))
    return len(representatives)


def auto_voltage_threshold(
    buses: list[dict],
    lines: list[dict],
    max_buses: int = AUTO_MAX_HV_BUSES,
) -> float:
    """Compute the lowest voltage threshold [kV] that keeps <= *max_buses* visible.

    The function iterates over all distinct voltage levels found in the bus
    array (from highest to lowest) and returns the **smallest** threshold that
    still produces at most *max_buses* representative buses after reduction.

    If no threshold achieves the target (e.g. all buses have the same voltage
    level) the highest found voltage level is returned as a fallback so the
    diagram is at least somewhat reduced.

    Returns 0.0 when the total bus count is already <= *max_buses* (no
    reduction needed).
    """
    if len(buses) <= max_buses:
        return 0.0

    # Collect distinct voltage levels using safe .get() to avoid KeyError
    levels = sorted(
        {float(v) for b in buses if (v := b.get("voltage")) is not None},
    )
    if not levels:
        return 0.0

    # Find the smallest threshold (lowest voltage level) that hits the target.
    # We scan levels from highest to lowest: once we fall below the target,
    # the previous (higher) level is the answer.
    chosen = levels[-1]  # fallback: the highest voltage level found
    for lvl in reversed(levels):
        n = count_visible_buses(buses, lines, lvl)
        if n <= max_buses:
            chosen = lvl
        else:
            # going lower won't help -- stop searching downward
            break
    return chosen


def resolve_bus_ref(ref, vmap: dict):  # noqa: ANN001
    """Translate a bus reference through the voltage map (identity if absent)."""
    return vmap.get(ref, ref)
