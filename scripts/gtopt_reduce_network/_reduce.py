# SPDX-License-Identifier: BSD-3-Clause
"""Top-level ``reduce`` driver: orchestrates the load → simplify → cluster
→ aggregate → rewrite → save pipeline.

This module is intentionally CLI-agnostic: it takes a ``Case`` plus a
``ReduceConfig`` and returns a ``ReduceResult`` ready to be written to
disk. ``main.py`` wraps it with argparse and CSV/JSON dumps.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

from gtopt_reduce_network._aggregate import AggregatedLine, aggregate_lines
from gtopt_reduce_network._busmap import AggregatorRow, BusmapRow, LinemapRow
from gtopt_reduce_network._cluster import build_busmap, select_anchors
from gtopt_reduce_network._components import (
    expand_busmap_with_eliminated,
    rewrite_component_buses,
)
from gtopt_reduce_network._distance import (
    ptdf_distance_matrix,
    reactance_shortest_path_matrix,
    zbus_distance_matrix,
)
from gtopt_reduce_network._io import Case, resolve_bus_ref
from gtopt_reduce_network._local_simplify import simplify_local
from gtopt_reduce_network._partition_nx import build_busmap_nx
from gtopt_reduce_network._line_schedules import aggregate_line_schedules
from gtopt_reduce_network._simplify import apply_loss_mode, apply_transport_only
from gtopt_reduce_network._topology import LineGraph, build_line_graph

logger = logging.getLogger(__name__)


_LOSS_MODES = ("keep", "linear", "off", "uplift")
_COLLISION_MODES = ("replace", "add", "compound")
_PARTITIONS = ("hac", "louvain-mincut")
_NX_WEIGHTS = ("capacity", "susceptance")


@dataclass(slots=True)
class ReduceConfig:
    target_buses: int
    distance: str = "reactance-shortest-path"  # | "zbus" | "ptdf"
    reactance_rule: str = "series-parallel"
    user_anchor_uids: tuple[int, ...] = ()
    min_load_mw: float | None = None
    min_gen_capacity_mw: float | None = None
    include_reservoir_hosts: bool = True
    skip_local_simplify: bool = False
    drop_lines_below_mw: float | None = None
    integer_uid_offset: int = 1_000_000  # synth bus uid = offset + cluster idx
    # --- Partition backend -------------------------------------------------
    partition: str = "hac"  # hac | louvain-mincut
    nx_weight: str = "capacity"  # capacity | susceptance (louvain-mincut only)
    seed: int = 0  # louvain community seed (louvain-mincut only)
    # --- Transport / loss simplifications ---------------------------------
    transport_only: bool = False  # set options.use_kirchhoff=false
    loss_mode: str = "keep"  # keep | linear | off | uplift
    loss_uplift_pct: float = 3.0  # only used when loss_mode="uplift"
    loss_uplift_collision: str = "replace"  # replace | add | compound
    # When non-empty, also aggregate per-line parquet schedules
    # (Line/<field>.parquet) into sibling Line/<field>_<reduced_tag>.parquet
    # files; the reduced JSON's per-line field references the new stem.
    reduced_tag: str = ""
    # Directory containing the original case's Line/ subtree.  Defaults
    # to the parent dir of the input JSON; the cascade-reduced flow
    # passes it explicitly.
    parquet_case_dir: str | None = None

    def __post_init__(self) -> None:
        if self.loss_mode not in _LOSS_MODES:
            raise ValueError(
                f"loss_mode must be one of {_LOSS_MODES}, got {self.loss_mode!r}"
            )
        if self.loss_uplift_collision not in _COLLISION_MODES:
            raise ValueError(
                f"loss_uplift_collision must be one of {_COLLISION_MODES}, "
                f"got {self.loss_uplift_collision!r}"
            )
        if self.partition not in _PARTITIONS:
            raise ValueError(
                f"partition must be one of {_PARTITIONS}, got {self.partition!r}"
            )
        if self.nx_weight not in _NX_WEIGHTS:
            raise ValueError(
                f"nx_weight must be one of {_NX_WEIGHTS}, got {self.nx_weight!r}"
            )

    def as_dict(self) -> dict[str, Any]:
        return {
            "target_buses": self.target_buses,
            "distance": self.distance,
            "reactance_rule": self.reactance_rule,
            "user_anchor_uids": list(self.user_anchor_uids),
            "min_load_mw": self.min_load_mw,
            "min_gen_capacity_mw": self.min_gen_capacity_mw,
            "include_reservoir_hosts": self.include_reservoir_hosts,
            "skip_local_simplify": self.skip_local_simplify,
            "drop_lines_below_mw": self.drop_lines_below_mw,
            "integer_uid_offset": self.integer_uid_offset,
            "partition": self.partition,
            "nx_weight": self.nx_weight,
            "seed": self.seed,
            "transport_only": self.transport_only,
            "loss_mode": self.loss_mode,
            "loss_uplift_pct": self.loss_uplift_pct,
            "loss_uplift_collision": self.loss_uplift_collision,
            "reduced_tag": self.reduced_tag,
            "parquet_case_dir": self.parquet_case_dir,
        }


@dataclass(slots=True)
class ReduceResult:
    case: Case  # the reduced case (mutated copy)
    busmap: list[BusmapRow]
    linemap: list[LinemapRow]
    aggregator: list[AggregatorRow]
    aggregated_lines: list[AggregatedLine]
    eliminated_buses: list[int] = field(default_factory=list)
    anchor_uids: list[int] = field(default_factory=list)


def reduce_case(case: Case, config: ReduceConfig) -> ReduceResult:
    """Run the full reduction pipeline against ``case`` (not mutated)."""
    # Operate on a deep copy so the caller's Case is untouched.
    work = case.deepcopy()
    graph = build_line_graph(work)
    logger.info(
        "loaded %d buses, %d lines (%d skipped, %d DC)",
        graph.n_buses,
        graph.n_lines,
        len(graph.skipped_line_uids),
        len(graph.dc_line_uids),
    )

    # 1. Local simplify ----------------------------------------------------
    dc_mask = graph.line_is_dc
    assert dc_mask is not None
    simplified_buses = list(graph.bus_uids)
    surviving_uids = list(graph.line_uids)
    surviving_a_uids = [
        graph.bus_uids[int(graph.line_a[i])] for i in range(graph.n_lines)
    ]
    surviving_b_uids = [
        graph.bus_uids[int(graph.line_b[i])] for i in range(graph.n_lines)
    ]
    surviving_x = list(map(float, graph.line_x))
    surviving_r = list(map(float, graph.line_r))
    surviving_fab = list(map(float, graph.line_fab))
    surviving_fba = list(map(float, graph.line_fba))
    surviving_dc = [bool(v) for v in dc_mask]
    local_linemap: list[LinemapRow] = []
    eliminated_buses: list[int] = []
    parent_of_eliminated: dict[int, int] = {}

    if not config.skip_local_simplify:
        # DC lines are controllable devices, not impedances: exclude them
        # from the (exact-for-DC-OPF) X-based merges and pin their endpoint
        # buses so degree-2 elimination cannot swallow a converter station.
        injection_set = _injection_bus_set(work) | _dc_endpoint_bus_uids(graph)
        sr = simplify_local(_ac_only_graph(graph), injection_bus_uids=injection_set)
        # Track parent-of-eliminated for busmap expansion below.
        # The simplest assignment: each eliminated bus points to whichever
        # surviving bus appears first in its outgoing series-merged lines.
        parent_of_eliminated = _eliminated_to_parent(graph, sr.eliminated_buses)
        simplified_buses = list(sr.surviving_buses)
        surviving_uids = list(sr.surviving_line_uids)
        surviving_a_uids = list(sr.surviving_a)
        surviving_b_uids = list(sr.surviving_b)
        surviving_x = list(sr.surviving_x)
        surviving_r = list(sr.surviving_r)
        surviving_fab = list(sr.surviving_fab)
        surviving_fba = list(sr.surviving_fba)
        surviving_dc = [False] * len(surviving_uids)
        for pos in range(graph.n_lines):  # re-append the untouched DC lines
            if not dc_mask[pos]:
                continue
            surviving_uids.append(graph.line_uids[pos])
            surviving_a_uids.append(graph.bus_uids[int(graph.line_a[pos])])
            surviving_b_uids.append(graph.bus_uids[int(graph.line_b[pos])])
            surviving_x.append(float("inf"))
            surviving_r.append(float(graph.line_r[pos]))
            surviving_fab.append(float(graph.line_fab[pos]))
            surviving_fba.append(float(graph.line_fba[pos]))
            surviving_dc.append(True)
        local_linemap = list(sr.linemap)
        eliminated_buses = list(sr.eliminated_buses)
        logger.info(
            "local-simplify: %d→%d buses, %d→%d lines",
            graph.n_buses,
            len(simplified_buses),
            graph.n_lines,
            len(surviving_uids),
        )

    # 2. Sub-graph over surviving buses/lines ------------------------------
    sub_graph = LineGraph(
        bus_uids=simplified_buses,
        bus_index={u: i for i, u in enumerate(simplified_buses)},
        line_uids=surviving_uids,
        line_a=np.asarray(
            [simplified_buses.index(b) for b in surviving_a_uids], dtype=np.int64
        ),
        line_b=np.asarray(
            [simplified_buses.index(b) for b in surviving_b_uids], dtype=np.int64
        ),
        line_x=np.asarray(surviving_x, dtype=float),
        line_r=np.asarray(surviving_r, dtype=float),
        line_fab=np.asarray(surviving_fab, dtype=float),
        line_fba=np.asarray(surviving_fba, dtype=float),
        line_is_dc=np.asarray(surviving_dc, dtype=bool),
    )

    # 3. Anchors + cluster -------------------------------------------------
    anchor_sel = select_anchors(
        work,
        target_buses=config.target_buses,
        surviving_bus_uids=simplified_buses,
        user_anchor_uids=config.user_anchor_uids,
        min_load_mw=config.min_load_mw,
        min_gen_capacity_mw=config.min_gen_capacity_mw,
        include_reservoir_hosts=config.include_reservoir_hosts,
    )
    if config.partition == "louvain-mincut":
        busmap = build_busmap_nx(
            sub_graph,
            target_buses=config.target_buses,
            anchors=anchor_sel.bus_uids,
            weight_mode=config.nx_weight,
            seed=config.seed,
        )
    else:
        # The distance matrix is only needed by the HAC path (the ptdf
        # metric in particular is dense and expensive — skip when unused).
        distance = _distance_matrix(sub_graph, config.distance)
        busmap = build_busmap(
            simplified_buses,
            distance,
            target_buses=config.target_buses,
            anchors=anchor_sel.bus_uids,
        )
    cluster_of_bus_simplified = {
        row.original_bus_uid: row.cluster_bus_uid for row in busmap
    }

    # 4. Expand busmap to cover eliminated buses too ----------------------
    full_busmap = expand_busmap_with_eliminated(
        busmap,
        eliminated_buses,
        parent_bus_of_eliminated=parent_of_eliminated,
    )
    cluster_of_bus_full = {
        row.original_bus_uid: row.cluster_bus_uid for row in full_busmap
    }

    # 5. Aggregate inter-cluster lines ------------------------------------
    next_uid = (max(graph.line_uids) if graph.line_uids else 0) + 100_001
    agg_lines, agg_linemap = aggregate_lines(
        surviving_uids,
        surviving_a_uids,
        surviving_b_uids,
        surviving_x,
        surviving_r,
        surviving_fab,
        surviving_fba,
        cluster_of_bus_simplified,
        rule=config.reactance_rule,
        next_line_uid=next_uid,
        surviving_is_dc=surviving_dc,
    )

    # Optional cap pruning.
    if config.drop_lines_below_mw is not None:
        thr = float(config.drop_lines_below_mw)
        agg_lines = [ln for ln in agg_lines if max(ln.tmax_ab, ln.tmax_ba) >= thr]

    # 6. Build the reduced JSON system arrays ------------------------------
    # Snapshot the ORIGINAL line_array before it gets replaced below — the
    # schedule aggregator needs scalar fallback values keyed by original uid.
    original_line_array = list(work.system.get("line_array", []))
    cluster_bus_uids = sorted({row.cluster_bus_uid for row in full_busmap})
    work.system["bus_array"] = _build_bus_array(
        work, cluster_bus_uids, work.bus_name_by_uid
    )
    work.system["line_array"] = _build_line_array(agg_lines, work.bus_name_by_uid)

    # 7. Rewrite component bus refs ---------------------------------------
    aggregator = rewrite_component_buses(
        work,
        cluster_of_bus_full,
        bus_name_by_uid={
            u: work.bus_name_by_uid.get(u, f"bus_{u}") for u in cluster_bus_uids
        },
    )

    # Reindex bus name maps for the reduced case.
    work.bus_uid_by_name = {
        n: u for u, n in work.bus_name_by_uid.items() if u in set(cluster_bus_uids)
    }
    work.bus_name_by_uid = {
        u: n for u, n in work.bus_name_by_uid.items() if u in set(cluster_bus_uids)
    }

    # 8. Optional model-side simplifications ------------------------------
    if config.transport_only:
        apply_transport_only(work)
    if config.loss_mode != "keep":
        apply_loss_mode(
            work,
            config.loss_mode,
            config.loss_uplift_pct,
            collision=config.loss_uplift_collision,
        )

    # 9. Optional per-line parquet schedule aggregation -------------------
    if config.reduced_tag and config.parquet_case_dir:
        input_directory = work.options.get("input_directory", ".") or "."
        ac_agg_lines = [ln for ln in agg_lines if not ln.is_dc]
        if len(ac_agg_lines) != len(agg_lines):
            logger.info(
                "line-schedule: %d DC equivalent lines keep their scalar "
                "fields (schedule aggregation covers AC corridors only)",
                len(agg_lines) - len(ac_agg_lines),
            )
        field_to_stem = aggregate_line_schedules(
            case_dir=Path(config.parquet_case_dir),
            input_directory=str(input_directory),
            reduced_tag=config.reduced_tag,
            aggregated_lines=ac_agg_lines,
            original_line_array=original_line_array,
        )
        # Rewrite each reduced line's JSON entry: replace the scalar value
        # of any aggregated field with the new parquet stem.  DC equivalent
        # lines keep their scalar fields (no X schedules to point at).
        if field_to_stem:
            for ln in work.array("line_array"):
                if ln.get("type") == "dc":
                    continue
                for field, new_stem in field_to_stem.items():
                    ln[field] = new_stem
            logger.info(
                "line-schedule: rewrote %d reduced line entries with stems for %s",
                len(work.array("line_array")),
                list(field_to_stem),
            )

    return ReduceResult(
        case=work,
        busmap=full_busmap,
        linemap=local_linemap + agg_linemap,
        aggregator=aggregator,
        aggregated_lines=agg_lines,
        eliminated_buses=eliminated_buses,
        anchor_uids=anchor_sel.bus_uids,
    )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _distance_matrix(graph: LineGraph, kind: str) -> np.ndarray:
    if kind == "reactance-shortest-path":
        return reactance_shortest_path_matrix(graph)
    if kind == "zbus":
        return zbus_distance_matrix(graph)
    if kind == "ptdf":
        return ptdf_distance_matrix(graph)
    raise ValueError(
        f"unknown distance metric {kind!r}; "
        "supported: reactance-shortest-path | zbus | ptdf"
    )


def _ac_only_graph(graph: LineGraph) -> LineGraph:
    """Copy of ``graph`` with the DC lines removed (all buses kept)."""
    mask = graph.line_is_dc
    assert mask is not None
    keep = [p for p in range(graph.n_lines) if not mask[p]]
    return LineGraph(
        bus_uids=list(graph.bus_uids),
        bus_index=dict(graph.bus_index),
        line_uids=[graph.line_uids[p] for p in keep],
        line_a=graph.line_a[keep],
        line_b=graph.line_b[keep],
        line_x=graph.line_x[keep],
        line_r=graph.line_r[keep],
        line_fab=graph.line_fab[keep],
        line_fba=graph.line_fba[keep],
    )


def _dc_endpoint_bus_uids(graph: LineGraph) -> set[int]:
    """Bus uids hosting a DC-line terminal (converter stations)."""
    mask = graph.line_is_dc
    assert mask is not None
    out: set[int] = set()
    for pos in range(graph.n_lines):
        if mask[pos]:
            out.add(graph.bus_uids[int(graph.line_a[pos])])
            out.add(graph.bus_uids[int(graph.line_b[pos])])
    return out


def _injection_bus_set(case: Case) -> set[int]:
    out: set[int] = set()
    for arr in (
        "generator_array",
        "demand_array",
        "battery_array",
        "turbine_array",
    ):
        for elem in case.array(arr):
            try:
                bus_uid = resolve_bus_ref(case, elem.get("bus"))
            except (KeyError, TypeError):
                continue
            if bus_uid is not None:
                out.add(bus_uid)
    return out


def _eliminated_to_parent(
    graph: LineGraph, eliminated_buses: list[int]
) -> dict[int, int]:
    """Map each eliminated bus to a surviving neighbour via its incidence."""
    out: dict[int, int] = {}
    incident: dict[int, list[int]] = {u: [] for u in graph.bus_uids}
    for pos in range(graph.n_lines):
        a = graph.bus_uids[int(graph.line_a[pos])]
        b = graph.bus_uids[int(graph.line_b[pos])]
        incident[a].append(b)
        incident[b].append(a)
    elim_set = set(int(u) for u in eliminated_buses)
    for eb in eliminated_buses:
        for nb in incident.get(int(eb), ()):
            if nb not in elim_set:
                out[int(eb)] = int(nb)
                break
    return out


def _build_bus_array(
    case: Case, cluster_bus_uids: list[int], name_by_uid: dict[int, str]
) -> list[dict[str, Any]]:
    # Preserve any optional fields (voltage, region, etc.) from the original
    # entries when they survive; new aggregated buses get a minimal dict.
    by_uid = {int(b["uid"]): b for b in case.array("bus_array")}
    out: list[dict[str, Any]] = []
    for u in cluster_bus_uids:
        if u in by_uid:
            out.append(dict(by_uid[u]))
        else:
            out.append({"uid": int(u), "name": name_by_uid.get(u, f"bus_{u}")})
    return out


def _build_line_array(
    agg_lines: list[AggregatedLine], name_by_uid: dict[int, str]
) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for ln in agg_lines:
        entry: dict[str, Any] = {
            "uid": ln.uid,
            "name": ln.name,
            "bus_a": name_by_uid.get(ln.bus_a_uid, ln.bus_a_uid),
            "bus_b": name_by_uid.get(ln.bus_b_uid, ln.bus_b_uid),
            "tmax_ab": ln.tmax_ab,
            "tmax_ba": ln.tmax_ba,
        }
        if ln.is_dc:
            # DC equivalent: no reactance — gtopt's KVL emitter skips the
            # line, leaving pure transport capacity (kirchhoff_node_angle).
            entry["type"] = "dc"
            if ln.resistance > 0.0:
                entry["resistance"] = ln.resistance
        else:
            entry["reactance"] = ln.reactance
            entry["resistance"] = ln.resistance
        out.append(entry)
    return out
