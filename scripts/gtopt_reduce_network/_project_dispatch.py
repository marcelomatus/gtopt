# SPDX-License-Identifier: BSD-3-Clause
"""Project dispatch parquets from the reduced solution back to the original
nodal grid.

The reduced run writes per-element results indexed by the *cluster* bus
or the *aggregated* line uid. To make those results comparable with the
original nodal model we walk the busmap and aggregator table and rewrite
the relevant uid columns. For per-unit dispatch (Generator output) the
projection is a no-op on the *value* — only the bus column changes. For
load (Demand) and line flows the value is split across the original
sub-elements.

Schema and file layout follow ``scripts/gtopt_check_output/_reader.py``:
Hive-partitioned parquet ``{kind}/{name}.parquet/scene=*/phase=*/part.parquet``
or per-(scene,phase) CSV shards.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Iterable

import pandas as pd

from gtopt_reduce_network._busmap import (
    AggregatorRow,
    BusmapRow,
    LinemapRow,
    load_aggregator,
    load_busmap,
    load_linemap,
)

logger = logging.getLogger(__name__)


def project_results(
    reduced_dir: str | Path,
    *,
    busmap: list[BusmapRow] | str | Path,
    linemap: list[LinemapRow] | str | Path,
    aggregator: list[AggregatorRow] | str | Path,
    output_dir: str | Path,
) -> dict[str, list[Path]]:
    """Walk the reduced output tree and emit projected nodal parquets.

    Returns a dict ``{kind: [output_paths]}``.
    """
    busmap_rows = busmap if isinstance(busmap, list) else load_busmap(busmap)
    linemap_rows = linemap if isinstance(linemap, list) else load_linemap(linemap)
    agg_rows = (
        aggregator if isinstance(aggregator, list) else load_aggregator(aggregator)
    )

    src = Path(reduced_dir)
    dst = Path(output_dir)
    dst.mkdir(parents=True, exist_ok=True)

    # Build the lookup tables.
    cluster_of_bus = {r.original_bus_uid: r.cluster_bus_uid for r in busmap_rows}
    bus_members: dict[int, list[int]] = {}
    for orig, clu in cluster_of_bus.items():
        bus_members.setdefault(clu, []).append(orig)

    inter_cluster_lines = [r for r in linemap_rows if r.rule == "inter-cluster"]
    line_members: dict[int, list[int]] = {}
    for r in inter_cluster_lines:
        if r.equivalent_line_uid is None:
            continue
        line_members.setdefault(r.equivalent_line_uid, []).append(r.original_line_uid)

    component_share: dict[tuple[str, int], list[tuple[int, float]]] = {}
    for ar in agg_rows:
        component_share.setdefault((ar.component_kind, ar.component_uid), []).append(
            (ar.original_bus_uid, ar.share)
        )

    out: dict[str, list[Path]] = {}

    # Generators / Demands / Batteries — values stay nodal; we rewrite the
    # bus index column when present.
    for kind in ("Generator", "Demand", "Battery"):
        for src_path in _list_parquets(src / kind):
            df = pd.read_parquet(src_path)
            df_out = _rewrite_bus_column(df, kind.lower(), component_share)
            out_path = dst / src_path.relative_to(src)
            out_path.parent.mkdir(parents=True, exist_ok=True)
            df_out.to_parquet(out_path, compression="snappy")
            out.setdefault(kind, []).append(out_path)

    # Line flows — each aggregated line carries the sum of original flows;
    # split equally by capacity share for v1 (PTDF-fit split deferred).
    for src_path in _list_parquets(src / "Line"):
        df = pd.read_parquet(src_path)
        df_out = _split_line_column(df, line_members)
        out_path = dst / src_path.relative_to(src)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        df_out.to_parquet(out_path, compression="snappy")
        out.setdefault("Line", []).append(out_path)

    # LMPs — copy cluster's λ to every original bus.
    for src_path in _list_parquets(src / "Bus"):
        df = pd.read_parquet(src_path)
        df_out = _broadcast_bus_column(df, bus_members)
        out_path = dst / src_path.relative_to(src)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        df_out.to_parquet(out_path, compression="snappy")
        out.setdefault("Bus", []).append(out_path)

    return out


# ---------------------------------------------------------------------------


def _list_parquets(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return sorted(root.rglob("*.parquet"))


def _rewrite_bus_column(
    df: pd.DataFrame,
    kind: str,
    component_share: dict[tuple[str, int], list[tuple[int, float]]],
) -> pd.DataFrame:
    """Rewrite the per-component bus column from cluster_uid back to original.

    Pure passthrough if the dataframe lacks a ``bus`` column or the kind
    has no entries in ``component_share`` (e.g. Battery may not be merged).
    """
    if "bus" not in df.columns and "bus_uid" not in df.columns:
        return df
    bus_col = "bus" if "bus" in df.columns else "bus_uid"
    uid_col = "uid" if "uid" in df.columns else None
    if uid_col is None:
        return df
    rows: list[pd.Series] = []
    for _, row in df.iterrows():
        comp_uid = int(row[uid_col])
        share_list = component_share.get((kind, comp_uid))
        if not share_list or len(share_list) == 1:
            if share_list and len(share_list) == 1:
                row = row.copy()
                row[bus_col] = share_list[0][0]
            rows.append(row)
            continue
        for orig_bus, share in share_list:
            new_row = row.copy()
            new_row[bus_col] = orig_bus
            for col in df.select_dtypes(include="number").columns:
                if col in (uid_col, bus_col):
                    continue
                new_row[col] = float(row[col]) * float(share)
            rows.append(new_row)
    return pd.DataFrame(rows, columns=df.columns).reset_index(drop=True)


def _split_line_column(
    df: pd.DataFrame, line_members: dict[int, list[int]]
) -> pd.DataFrame:
    if "uid" not in df.columns:
        return df
    rows: list[pd.Series] = []
    for _, row in df.iterrows():
        line_uid = int(row["uid"])
        members = line_members.get(line_uid)
        if not members:
            rows.append(row)
            continue
        share = 1.0 / float(len(members))
        for orig_uid in members:
            new_row = row.copy()
            new_row["uid"] = orig_uid
            for col in df.select_dtypes(include="number").columns:
                if col == "uid":
                    continue
                new_row[col] = float(row[col]) * share
            rows.append(new_row)
    return pd.DataFrame(rows, columns=df.columns).reset_index(drop=True)


def _broadcast_bus_column(
    df: pd.DataFrame, bus_members: dict[int, list[int]]
) -> pd.DataFrame:
    if "uid" not in df.columns:
        return df
    rows: list[pd.Series] = []
    for _, row in df.iterrows():
        cluster_uid = int(row["uid"])
        members: Iterable[int] = bus_members.get(cluster_uid, [cluster_uid])
        for orig in members:
            new_row = row.copy()
            new_row["uid"] = orig
            rows.append(new_row)
    return pd.DataFrame(rows, columns=df.columns).reset_index(drop=True)
