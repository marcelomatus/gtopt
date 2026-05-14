# SPDX-License-Identifier: BSD-3-Clause
"""Project capacity-expansion outcomes from reduced → nodal.

Inputs:

* ``reduced_investment.json`` — per-cluster ``Δ`` for new generators,
  new lines, new batteries (whatever the reduced LP returned).
* ``busmap.csv``, ``aggregator_table.csv``, ``linemap.csv`` — the audit
  trail emitted by the reducer.
* original case JSON — needed for per-bus shares (existing capacity,
  load, resource quality).

Outputs:

* a patch JSON merging the per-bus ``expmod`` / ``expcap`` increments
  back into the original case structure;
* ``projected_investment.csv`` — audit row per (component, original_bus,
  delta).
"""

from __future__ import annotations

import csv
import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from gtopt_reduce_network._busmap import (
    AggregatorRow,
    BusmapRow,
    LinemapRow,
    load_aggregator,
    load_busmap,
    load_linemap,
)
from gtopt_reduce_network._io import Case, load_case, resolve_bus_ref

logger = logging.getLogger(__name__)

ShareMode = str  # "existing" | "load" | "resource" | "nodal_lp" | "manual"


@dataclass(slots=True)
class ProjectInvestmentConfig:
    share_mode: ShareMode = "existing"
    share_csv: str | Path | None = None  # required if share_mode="manual"
    # For new lines: how to split capacity across original corridors.
    line_share_mode: str = "capacity"  # | "loading" (loading needs flow data)


def project_investment(
    reduced_investment: dict[str, Any],
    *,
    original_case: Case | str | Path,
    busmap: list[BusmapRow] | str | Path,
    aggregator: list[AggregatorRow] | str | Path,
    linemap: list[LinemapRow] | str | Path,
    config: ProjectInvestmentConfig,
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Project per-cluster expansion deltas to per-bus deltas.

    Returns ``(patch_json, audit_rows)`` where ``patch_json`` mirrors a
    gtopt case structure (only the deltas) and ``audit_rows`` lists every
    per-bus assignment.
    """
    case = (
        original_case if isinstance(original_case, Case) else load_case(original_case)
    )
    busmap_rows = busmap if isinstance(busmap, list) else load_busmap(busmap)
    agg_rows = (
        aggregator if isinstance(aggregator, list) else load_aggregator(aggregator)
    )
    linemap_rows = linemap if isinstance(linemap, list) else load_linemap(linemap)
    cluster_of_bus = {r.original_bus_uid: r.cluster_bus_uid for r in busmap_rows}
    members_of_cluster: dict[int, list[int]] = {}
    for orig, clu in cluster_of_bus.items():
        members_of_cluster.setdefault(clu, []).append(orig)

    share_fn = _share_function(case, config, members_of_cluster, agg_rows)
    audit: list[dict[str, Any]] = []
    patch_system: dict[str, list[dict[str, Any]]] = {}

    # 1. Generators: split per-cluster Δ across cluster member buses.
    for entry in reduced_investment.get("generator_array", []):
        cluster_uid = _cluster_uid_of_entry(entry, case)
        delta = float(entry.get("expmod_delta", 0.0))
        if delta <= 0.0:
            continue
        members = members_of_cluster.get(cluster_uid, [cluster_uid])
        weights = [share_fn("generator", m, entry) for m in members]
        deltas = _largest_remainder_split(
            delta, weights, integer=bool(entry.get("integer_expmod"))
        )
        for m, d in zip(members, deltas):
            if d <= 0:
                continue
            patch_system.setdefault("generator_array", []).append(
                {
                    "based_on": entry.get("uid"),
                    "bus": m,
                    "expmod_delta": d,
                }
            )
            audit.append(
                {
                    "kind": "generator",
                    "based_on": entry.get("uid"),
                    "cluster_bus": cluster_uid,
                    "original_bus": m,
                    "delta": d,
                }
            )

    # 2. Lines: split per-corridor Δ across original line members.
    by_corridor = _line_corridor_index(linemap_rows)
    for entry in reduced_investment.get("line_array", []):
        agg_uid = int(entry.get("uid"))
        delta = float(entry.get("expmod_delta", 0.0))
        if delta <= 0.0:
            continue
        original_uids = by_corridor.get(agg_uid, [])
        if not original_uids:
            continue
        weights = _line_weights(case, original_uids, mode=config.line_share_mode)
        deltas = _largest_remainder_split(
            delta, weights, integer=bool(entry.get("integer_expmod"))
        )
        for u, d in zip(original_uids, deltas):
            if d <= 0:
                continue
            patch_system.setdefault("line_array", []).append(
                {"uid": u, "expmod_delta": d}
            )
            audit.append(
                {
                    "kind": "line",
                    "based_on": agg_uid,
                    "original_line": u,
                    "delta": d,
                }
            )

    return {"system": patch_system}, audit


def write_audit_csv(rows: list[dict[str, Any]], path: str | Path) -> None:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    if not rows:
        p.write_text("", encoding="utf-8")
        return
    fieldnames = sorted({k for r in rows for k in r.keys()})
    with p.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


# ---------------------------------------------------------------------------


def _cluster_uid_of_entry(entry: dict[str, Any], case: Case) -> int:
    bus = entry.get("bus")
    if isinstance(bus, int):
        return bus
    if isinstance(bus, str):
        try:
            return resolve_bus_ref(case, bus) or 0
        except KeyError:
            return 0
    raise ValueError(f"investment entry missing bus ref: {entry!r}")


def _line_corridor_index(linemap: list[LinemapRow]) -> dict[int, list[int]]:
    out: dict[int, list[int]] = {}
    for row in linemap:
        if row.rule == "inter-cluster" and row.equivalent_line_uid is not None:
            out.setdefault(row.equivalent_line_uid, []).append(row.original_line_uid)
    return out


def _line_weights(case: Case, line_uids: list[int], *, mode: str) -> list[float]:
    if mode == "capacity":
        by_uid = {int(ln["uid"]): ln for ln in case.array("line_array")}
        return [
            float(by_uid.get(u, {}).get("tmax_ab", 0.0) or 0.0) or 1.0
            for u in line_uids
        ]
    raise ValueError(f"unsupported line_share_mode={mode!r}; supported: capacity")


def _share_function(
    case: Case,
    config: ProjectInvestmentConfig,
    members_of_cluster: dict[int, list[int]],
    aggregator_rows: list[AggregatorRow],
) -> Callable[[str, int, dict[str, Any]], float]:
    """Build a callable returning the share weight for (kind, bus, entry)."""
    mode = config.share_mode
    if mode == "existing":
        existing = _existing_capacity_by_bus(case)
        return lambda kind, bus, entry: float(existing.get((kind, bus), 0.0))
    if mode == "load":
        loads = _peak_load_by_bus(case)
        return lambda kind, bus, entry: float(loads.get(bus, 0.0))
    if mode == "resource":
        # Without external resource data, fall back to existing capacity.
        existing = _existing_capacity_by_bus(case)
        return lambda kind, bus, entry: float(existing.get((kind, bus), 0.0)) or 1.0
    if mode == "manual":
        if config.share_csv is None:
            raise ValueError("share_mode='manual' requires share_csv")
        manual = _read_manual_shares(config.share_csv)
        return lambda kind, bus, entry: float(manual.get((kind, bus), 0.0))
    if mode == "nodal_lp":
        # v1 falls back to the existing-capacity rule and warns.
        logger.warning(
            "share_mode='nodal_lp' not yet implemented; using 'existing' rule"
        )
        existing = _existing_capacity_by_bus(case)
        return lambda kind, bus, entry: float(existing.get((kind, bus), 0.0))
    raise ValueError(f"unknown share_mode={mode!r}")


def _existing_capacity_by_bus(case: Case) -> dict[tuple[str, int], float]:
    out: dict[tuple[str, int], float] = {}
    for arr_name, kind in (
        ("generator_array", "generator"),
        ("battery_array", "battery"),
    ):
        for elem in case.array(arr_name):
            try:
                bus_uid = resolve_bus_ref(case, elem.get("bus"))
            except (KeyError, TypeError):
                continue
            if bus_uid is None:
                continue
            cap = elem.get("capacity") or elem.get("pmax") or 0.0
            if isinstance(cap, (int, float)) and not isinstance(cap, bool):
                out[(kind, bus_uid)] = out.get((kind, bus_uid), 0.0) + float(cap)
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


def _scalar_peak(value: object) -> float:
    if isinstance(value, bool):
        return 0.0
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
        return max(flat) if flat else 0.0
    return 0.0


def _read_manual_shares(path: str | Path) -> dict[tuple[str, int], float]:
    """Read a per-bus share CSV. Expected columns: kind, bus_uid, weight."""
    out: dict[tuple[str, int], float] = {}
    with Path(path).open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            kind = row["kind"].strip()
            bus_uid = int(row["bus_uid"])
            weight = float(row["weight"])
            out[(kind, bus_uid)] = weight
    return out


def _largest_remainder_split(
    total: float, weights: list[float], *, integer: bool
) -> list[float]:
    """Split ``total`` across ``weights`` preserving the sum.

    When ``integer`` is True, applies the largest-remainder method
    (Hamilton's seat allocation) so that ``Σ rounded == round(total)``.
    """
    if not weights or total <= 0:
        return [0.0] * len(weights)
    s = float(sum(weights))
    if s <= 0:
        # All-zero weights: split evenly.
        out = [total / len(weights)] * len(weights)
    else:
        out = [total * float(w) / s for w in weights]
    if not integer:
        return out
    floor = [int(x) for x in out]
    remainder = [x - f for x, f in zip(out, floor)]
    deficit = int(round(total)) - sum(floor)
    order = sorted(range(len(out)), key=lambda i: remainder[i], reverse=True)
    for i in order[: max(0, deficit)]:
        floor[i] += 1
    return [float(f) for f in floor]


def write_patch_json(patch: dict[str, Any], path: str | Path) -> None:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", encoding="utf-8") as f:
        json.dump(patch, f, indent=2)
        f.write("\n")
