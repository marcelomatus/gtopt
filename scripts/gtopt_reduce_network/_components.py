# SPDX-License-Identifier: BSD-3-Clause
"""Rewrite element bus references after the busmap is fixed.

For each Generator / Demand / Battery / Turbine the ``bus`` reference
is rewritten to its cluster representative. Hydro topology
(Junction / Waterway / Reservoir) is left untouched: only the
electrical injection bus moves. Each rewrite is recorded in the
aggregator table so the projector can split cluster-level results back
to the original buses.
"""

from __future__ import annotations

from typing import Any, Iterable

from gtopt_reduce_network._busmap import AggregatorRow
from gtopt_reduce_network._io import Case, iter_bus_referencing_elements


def rewrite_component_buses(
    case: Case,
    busmap: dict[int, int],
    *,
    bus_name_by_uid: dict[int, str],
) -> list[AggregatorRow]:
    """Rewrite bus refs in-place; return the audit table.

    ``busmap`` is ``original_bus_uid -> cluster_bus_uid``.
    ``bus_name_by_uid`` is the *post-reduction* name lookup so we can
    write the new ``bus`` field as a name (the gtopt convention used in
    most cases; uid is also accepted).
    """
    rows: list[AggregatorRow] = []
    for arr_name, elem, field_name in iter_bus_referencing_elements(case):
        ref = elem[field_name]
        original_uid = _resolve_to_uid(case, ref)
        if original_uid is None:
            continue
        cluster_uid = busmap.get(original_uid)
        if cluster_uid is None:
            # Bus eliminated by local-simplify before clustering: pass the
            # element through unchanged (handler must already have moved
            # it to a surviving bus during simplification — the busmap
            # only covers surviving buses).
            continue
        if cluster_uid != original_uid:
            # Preserve the original ref form: int → int uid, str → name.
            if isinstance(ref, str):
                elem[field_name] = bus_name_by_uid.get(cluster_uid, str(cluster_uid))
            else:
                elem[field_name] = int(cluster_uid)
        rows.append(
            AggregatorRow(
                component_kind=_kind_from_array(arr_name),
                component_uid=int(elem.get("uid", 0)),
                original_bus_uid=original_uid,
                cluster_bus_uid=cluster_uid,
                share=1.0,
            )
        )
    return rows


def expand_busmap_with_eliminated(
    busmap: list[Any],  # list of BusmapRow
    eliminated_buses: Iterable[int],
    *,
    parent_bus_of_eliminated: dict[int, int],
) -> list[Any]:
    """Append entries for buses removed by local-simplify.

    ``parent_bus_of_eliminated`` maps each eliminated bus uid to the
    surviving neighbour it should be considered part of (for component
    purposes). The merged bus may itself sit in a cluster, so we route
    eliminated → parent → cluster.
    """
    from gtopt_reduce_network._busmap import BusmapRow  # local import

    cluster_of: dict[int, int] = {
        row.original_bus_uid: row.cluster_bus_uid for row in busmap
    }
    out = list(busmap)
    for eb in eliminated_buses:
        parent = parent_bus_of_eliminated.get(int(eb))
        if parent is None:
            continue
        if parent not in cluster_of:
            continue
        out.append(
            BusmapRow(original_bus_uid=int(eb), cluster_bus_uid=cluster_of[parent])
        )
    return out


def _resolve_to_uid(case: Case, ref: Any) -> int | None:
    if ref is None:
        return None
    if isinstance(ref, bool):
        return None
    if isinstance(ref, int):
        return ref if ref in case.bus_name_by_uid else None
    if isinstance(ref, str):
        return case.bus_uid_by_name.get(ref)
    return None


def _kind_from_array(arr_name: str) -> str:
    return arr_name.removesuffix("_array")
