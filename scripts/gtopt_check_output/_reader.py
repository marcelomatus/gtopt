# SPDX-License-Identifier: BSD-3-Clause
"""Read gtopt output and input files into pandas DataFrames."""

from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Iterator

import pandas as pd
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.dataset as pads

log = logging.getLogger(__name__)


def read_table(directory: Path, stem: str) -> pd.DataFrame | None:
    """Read a parquet or CSV table from *directory*/*stem*.{ext}.

    Handles three layouts transparently:

    * Legacy single file: ``{stem}.{ext}``.
    * Hive-partitioned parquet directory: ``{stem}.parquet/`` containing
      ``scene=<N>/phase=<M>/part.parquet`` — ``pd.read_parquet`` reads
      the whole dataset as one frame and surfaces ``scene`` and
      ``phase`` as partition columns.
    * CSV shards: ``{stem}_s*_p*.{csv,csv.zst,csv.gz}`` concatenated in
      sorted order (CSV has no dataset equivalent).

    Parquet is preferred.  Returns ``None`` if nothing matches.

    .. warning::
       On wide hive-partitioned datasets (1000+ columns × thousands of
       rows × hundreds of partitions) this materialises the *entire*
       table into pandas.  For a typical gtopt 2-year run that's
       ~10 GB per stream — multiply by the half-dozen streams the
       check suite touches and you blow past 80 GB easily.  Use
       :func:`open_dataset` + the streaming helpers below
       (:func:`streaming_uid_sum`, :func:`streaming_uid_sum_per_col`,
       :func:`streaming_minmax_count_neg`) for those workloads.
       :func:`read_table` is fine for small per-element metadata
       tables (planning.json, solver_status.json, etc.).
    """
    pq_path = directory / (stem + ".parquet")
    if pq_path.is_dir() or pq_path.is_file():
        try:
            return pd.read_parquet(pq_path)
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            log.warning("failed to read %s: %s", pq_path, exc)
            return None

    parent = directory / Path(stem).parent
    name = Path(stem).name
    for ext in (".csv", ".csv.zst", ".csv.gz"):
        shards = sorted(parent.glob(f"{name}_s*_p*{ext}"))
        if shards:
            try:
                frames = [pd.read_csv(f) for f in shards]
                return pd.concat(frames, ignore_index=True) if frames else None
            except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                log.warning("failed to read CSV shards for %s: %s", stem, exc)
                return None

        fpath = directory / (stem + ext)
        if fpath.is_file():
            try:
                return pd.read_csv(fpath)
            except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
                log.warning("failed to read %s: %s", fpath, exc)
                return None
    return None


# ──────────────────────────────────────────────────────────────────────────
# Streaming readers for the wide per-(scene, phase) datasets.  None of these
# materialise the whole table in memory — they iterate the per-partition
# parquet files via pyarrow and aggregate column-wise.
# ──────────────────────────────────────────────────────────────────────────


def open_dataset(directory: Path, stem: str) -> pads.Dataset | None:
    """Open a parquet / CSV stream lazily.  Returns ``None`` if the
    stem is absent.  The returned :class:`pyarrow.dataset.Dataset` is
    the handle the streaming helpers iterate on — it does NOT load any
    data until :meth:`Dataset.scanner` or :meth:`to_batches` runs.

    Format precedence: parquet (hive-partitioned dir > single file) >
    per-(scene, phase) CSV shards.  Cases that set
    ``options.output_format = "csv"`` (e.g. the igtopt
    bat4b24 fixture) emit ``<stem>_s<scene>_p<phase>.csv`` instead of
    ``<stem>.parquet``; the check needs a CSV fallback so
    curtailment / fail / dual checks run regardless of the on-disk
    serialisation.
    """
    pq_path = directory / (stem + ".parquet")
    if pq_path.is_dir() or pq_path.is_file():
        try:
            if pq_path.is_dir():
                return pads.dataset(pq_path, format="parquet", partitioning="hive")
            return pads.dataset(pq_path, format="parquet")
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            log.warning("open_dataset: failed to open %s: %s", pq_path, exc)
            return None

    # CSV fallback — list every per-(scene, phase) shard and let
    # pyarrow union them into a single dataset.  The schema is
    # auto-detected; long-form CSVs carry the standard ``scenario,
    # stage, block, uid, value`` header, wide-form CSVs carry
    # ``scenario, stage, block, uid:N…`` instead.  Both shapes flow
    # through ``dataset_layout`` / ``streaming_sol_weighted_sum_per_uid``
    # unchanged.
    parent = directory / stem
    parent_stem = parent.name
    csv_shards = (
        sorted((directory / parent.parent.name).glob(f"{parent_stem}_s*_p*.csv"))
        if parent.parent != directory
        else sorted(directory.glob(f"{parent_stem}_s*_p*.csv"))
    )
    if not csv_shards:
        # Final fallback for the single-shard case (no scene/phase
        # split — e.g. monolithic single-stage runs).
        single_csv = directory / (stem + ".csv")
        if single_csv.is_file():
            csv_shards = [single_csv]
    if not csv_shards:
        return None
    try:
        return pads.dataset(csv_shards, format="csv")
    except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
        log.warning("open_dataset: failed to open CSV shards for %s: %s", stem, exc)
        return None


def dataset_uid_cols(dataset: pads.Dataset) -> list[str]:
    """Return the ``uid:*`` column names from a dataset's schema (no data
    read)."""
    return [n for n in dataset.schema.names if n.startswith("uid:")]


def dataset_layout(dataset: pads.Dataset) -> str:
    """Sniff the on-disk schema of *dataset*.

    Returns ``"long"`` when the dataset uses the 6-column
    ``(scenario, stage, block, uid, value)`` layout introduced
    2026-05-19 to shrink wide hive-partition outputs (set by
    ``options.output_layout: "long"``).  Returns ``"wide"`` (the
    historical default) when the dataset has one ``uid:N`` column per
    element.  The sniff is purely schema-based — no rows are read.

    Cheap test (`"uid" in dataset.schema.names`) instead of
    ``dataset_uid_cols`` is intentional: a wide dataset always has at
    least one ``uid:N`` column but never a bare ``uid`` column, and a
    long dataset always has the bare ``uid`` column.
    """
    names = set(dataset.schema.names)
    if "uid" in names and "value" in names:
        return "long"
    return "wide"


def _scan_batches(dataset: pads.Dataset, columns: list[str] | None) -> Iterator:
    """Yield record batches; clamp to the columns we asked for so the
    scanner reads minimal data off disk."""
    if dataset is None:
        return
    scanner = dataset.scanner(columns=columns) if columns else dataset.scanner()
    yield from scanner.to_batches()


def streaming_uid_sum(dataset: pads.Dataset | None) -> float:
    """Σ over every cell of every uid:* column, streaming per partition
    batch.  Used wherever the old code had ``_to_wide(df).sum().sum()``.

    Layout-aware: dispatches to a long-form aggregator
    (`scenario, stage, block, uid, value` schema) when the dataset was
    written with `output_layout: "long"`.  The wide-form aggregator
    iterates all `uid:N` columns; the long-form aggregator iterates the
    single `value` column.  Result is identical because dropped zeros
    are sum-invariant.
    """
    if dataset is None:
        return 0.0
    if dataset_layout(dataset) == "long":
        total = 0.0
        for batch in _scan_batches(dataset, ["value"]):
            s = pc.sum(batch["value"]).as_py()
            if s is not None:
                total += float(s)
        return total
    cols = dataset_uid_cols(dataset)
    if not cols:
        return 0.0
    total = 0.0
    for batch in _scan_batches(dataset, cols):
        for col in cols:
            s = pc.sum(batch[col]).as_py()
            if s is not None:
                total += float(s)
    return total


def streaming_uid_sum_per_col(
    dataset: pads.Dataset | None,
) -> dict[int, float]:
    """Σ per uid column, keyed by integer uid.  Streaming.

    Layout-aware: when the dataset is long-form, we group on the `uid`
    column using `pyarrow.compute.hash_aggregate`-style reduction
    (implemented here as a manual per-batch group-by because pyarrow's
    public group-by API requires `Table.group_by(...)` which materialises
    a full table).  When wide, we iterate `uid:N` columns.
    """
    if dataset is None:
        return {}
    if dataset_layout(dataset) == "long":
        out: dict[int, float] = {}
        for batch in _scan_batches(dataset, ["uid", "value"]):
            # Group-by via a single pyarrow table aggregation per batch.
            # Each batch is bounded by `scanner` (default 1 Mi rows) so
            # the per-batch table never blows memory.
            tbl = pa.table({"uid": batch["uid"], "value": batch["value"]})
            grouped = tbl.group_by("uid").aggregate([("value", "sum")])
            uids = grouped["uid"].to_pylist()
            sums = grouped["value_sum"].to_pylist()
            for u, s in zip(uids, sums):
                if u is None or s is None:
                    continue
                out[int(u)] = out.get(int(u), 0.0) + float(s)
        return out
    cols = dataset_uid_cols(dataset)
    out = {int(c.split(":")[1]): 0.0 for c in cols}
    for batch in _scan_batches(dataset, cols):
        for col in cols:
            s = pc.sum(batch[col]).as_py()
            if s is not None:
                out[int(col.split(":")[1])] += float(s)
    return out


def streaming_uid_abs_max(dataset: pads.Dataset | None) -> float:
    """Max |value| across every uid:* cell.  Streaming.

    Layout-aware: long-form scans only the `value` column.
    """
    if dataset is None:
        return 0.0
    if dataset_layout(dataset) == "long":
        m = 0.0
        for batch in _scan_batches(dataset, ["value"]):
            arr = pc.abs(batch["value"])
            mx = pc.max(arr).as_py()
            if mx is not None and mx > m:
                m = float(mx)
        return m
    cols = dataset_uid_cols(dataset)
    if not cols:
        return 0.0
    m = 0.0
    for batch in _scan_batches(dataset, cols):
        for col in cols:
            arr = pc.abs(batch[col])
            mx = pc.max(arr).as_py()
            if mx is not None and mx > m:
                m = float(mx)
    return m


def streaming_uid_stats(
    dataset: pads.Dataset | None,
) -> dict[str, float]:
    """Aggregate count / min / max / sum / mean / negative-count across
    every uid:* cell.  Mean is computed from sum / count to avoid
    needing a single-shot pyarrow ``mean`` over the whole table.

    Layout-aware.  Note for long form: `count` measures the number of
    non-zero rows actually present in the file — dropped zeros are not
    counted, so the long-form `mean` is the conditional mean given
    non-zero, not the population mean.  Wide form's `count` already
    behaves the same way (nulls drop), so both forms report the same
    statistic for any quantity that is "mostly zero" (which is the
    target case for the long encoding).
    """
    if dataset is None:
        return {}
    if dataset_layout(dataset) == "long":
        n = 0
        total = 0.0
        mn = float("inf")
        mx = float("-inf")
        n_neg = 0
        for batch in _scan_batches(dataset, ["value"]):
            arr_clean = pc.drop_null(batch["value"])
            cnt = pc.count(arr_clean).as_py() or 0
            if cnt == 0:
                continue
            n += int(cnt)
            s = pc.sum(arr_clean).as_py()
            if s is not None:
                total += float(s)
            cur_min = pc.min(arr_clean).as_py()
            if cur_min is not None and cur_min < mn:
                mn = float(cur_min)
            cur_max = pc.max(arr_clean).as_py()
            if cur_max is not None and cur_max > mx:
                mx = float(cur_max)
            neg = pc.sum(pc.less(arr_clean, 0.0)).as_py()
            if neg is not None:
                n_neg += int(neg)
        if n == 0:
            return {
                "count": 0,
                "sum": 0.0,
                "min": 0.0,
                "max": 0.0,
                "mean": 0.0,
                "n_neg": 0,
            }
        return {
            "count": n,
            "sum": total,
            "min": mn,
            "max": mx,
            "mean": total / n,
            "n_neg": n_neg,
        }
    cols = dataset_uid_cols(dataset)
    if not cols:
        return {}
    n = 0
    total = 0.0
    mn = float("inf")
    mx = float("-inf")
    n_neg = 0
    for batch in _scan_batches(dataset, cols):
        for col in cols:
            arr = batch[col]
            arr_clean = pc.drop_null(arr)
            cnt = pc.count(arr_clean).as_py() or 0
            if cnt == 0:
                continue
            n += int(cnt)
            s = pc.sum(arr_clean).as_py()
            if s is not None:
                total += float(s)
            cur_min = pc.min(arr_clean).as_py()
            if cur_min is not None and cur_min < mn:
                mn = float(cur_min)
            cur_max = pc.max(arr_clean).as_py()
            if cur_max is not None and cur_max > mx:
                mx = float(cur_max)
            neg = pc.sum(pc.less(arr_clean, 0.0)).as_py()
            if neg is not None:
                n_neg += int(neg)
    if n == 0:
        return {"count": 0, "sum": 0.0, "min": 0.0, "max": 0.0, "mean": 0.0, "n_neg": 0}
    return {
        "count": n,
        "sum": total,
        "min": mn,
        "max": mx,
        "mean": total / n,
        "n_neg": n_neg,
    }


def streaming_sol_weighted_sum_per_uid(
    dataset: pads.Dataset | None,
    block_durations: dict[int, float],
) -> dict[int, float]:
    """Σ over (s,t,b) of  value(s,t,b,uid) × duration(b) — per uid.
    Returns ``{uid: total_energy_or_cost_for_that_uid}``.  Streaming.

    Layout-aware: long-form path computes `value × duration[block]`
    per row and then groups on `uid` (per-batch); wide-form path
    iterates `uid:N` columns.
    """
    if dataset is None:
        return {}
    if dataset_layout(dataset) == "long":
        out: dict[int, float] = {}
        for batch in _scan_batches(dataset, ["block", "uid", "value"]):
            block_arr = batch["block"].to_pylist()
            dur = [
                block_durations.get(b if b is not None else 0, 1.0) for b in block_arr
            ]
            dur_arr = pa.array(dur, type=pa.float64())
            weighted = pc.multiply(batch["value"].cast("float64"), dur_arr)
            tbl = pa.table({"uid": batch["uid"], "weighted": weighted})
            grouped = tbl.group_by("uid").aggregate([("weighted", "sum")])
            uids = grouped["uid"].to_pylist()
            sums = grouped["weighted_sum"].to_pylist()
            for u, s in zip(uids, sums):
                if u is None or s is None:
                    continue
                out[int(u)] = out.get(int(u), 0.0) + float(s)
        return out
    cols = dataset_uid_cols(dataset)
    out = {int(c.split(":")[1]): 0.0 for c in cols}
    for batch in _scan_batches(dataset, cols + ["block"]):
        block_arr = batch["block"].to_pylist()
        dur = [block_durations.get(b if b is not None else 0, 1.0) for b in block_arr]
        dur_arr = pa.array(dur, type=pa.float64())
        for col in cols:
            weighted = pc.multiply(batch[col].cast("float64"), dur_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                out[int(col.split(":")[1])] += float(s)
    return out


def streaming_pairwise_weighted_sum(
    dataset_a: pads.Dataset | None,
    dataset_b: pads.Dataset | None,
    block_durations: dict[int, float],
) -> float:
    """Σ over (s,t,b,uid) of  a(s,t,b,uid) × b(s,t,b,uid) × duration(b).

    Used by `compute_cost_breakdown` to compute the generation cost as
    `srmc × generation × duration` without materialising either
    dataset as a wide pandas DataFrame.

    Both datasets must share the same hive partitioning + schema (which
    is the case by construction for `Generator/srmc_sol` and
    `Generator/generation_sol` — both are emitted from the same
    `STBIndexHolder` keyed on (scene, phase, stage, block, uid).
    We iterate (scene, phase) fragments in lockstep and assume the
    row ordering within each partition matches.  Per fragment we read
    only the uid columns + the block column, multiply elementwise, and
    sum.
    """
    if dataset_a is None or dataset_b is None:
        return 0.0

    # Long-form fast path: each row is `(scenario, stage, block, uid,
    # value)`.  Join on `(scenario, stage, block, uid)` per fragment
    # pair (both datasets share the hive partitioning, so paired
    # fragments cover the same scene/phase), then sum
    # `a.value × b.value × duration(block)`.
    layout_a = dataset_layout(dataset_a)
    layout_b = dataset_layout(dataset_b)
    if layout_a == "long" and layout_b == "long":

        def _path_key_long(frag) -> str:
            path = str(frag.path)
            idx = path.rfind(".parquet/")
            return path[idx + len(".parquet/") :] if idx >= 0 else path

        map_a = {_path_key_long(f): f for f in dataset_a.get_fragments()}
        total = 0.0
        for frag_b in dataset_b.get_fragments():
            key = _path_key_long(frag_b)
            frag_a = map_a.get(key)
            if frag_a is None:
                continue
            tbl_a = frag_a.to_table(
                columns=["scenario", "stage", "block", "uid", "value"]
            )
            tbl_b = frag_b.to_table(
                columns=["scenario", "stage", "block", "uid", "value"]
            )
            if tbl_a.num_rows == 0 or tbl_b.num_rows == 0:
                continue
            # Inner-join on the composite key.  Use pandas for the
            # join — pyarrow's `Table.join` is also available but
            # requires Acero (not always present); pandas merge is
            # universally available and the per-partition fragment is
            # small (≤ a few million rows).
            df_a = tbl_a.to_pandas()
            df_b = tbl_b.to_pandas()
            merged = df_a.merge(
                df_b,
                on=["scenario", "stage", "block", "uid"],
                suffixes=("_a", "_b"),
                how="inner",
            )
            if merged.empty:
                continue
            dur = merged["block"].map(lambda b: block_durations.get(int(b), 1.0))
            total += float((merged["value_a"] * merged["value_b"] * dur).sum())
        return total

    cols_a = set(dataset_uid_cols(dataset_a))
    cols_b = set(dataset_uid_cols(dataset_b))
    shared = sorted(cols_a & cols_b)
    if not shared:
        return 0.0

    # Iterate fragments in matched order.  `Dataset.get_fragments()`
    # returns one fragment per hive partition file; key fragments by
    # the relative path under `<stem>.parquet/` (e.g.
    # `scene=12/phase=49/part.parquet`) so the two datasets line up
    # regardless of fragment enumeration order.
    def _path_key(frag) -> str:
        path = str(frag.path)
        idx = path.rfind(".parquet/")
        return path[idx + len(".parquet/") :] if idx >= 0 else path

    map_a = {_path_key(f): f for f in dataset_a.get_fragments()}

    shared_set = set(shared)
    total = 0.0
    for frag_b in dataset_b.get_fragments():
        key = _path_key(frag_b)
        frag_a = map_a.get(key)
        if frag_a is None:
            continue
        # Each fragment carries its own physical schema: gtopt only
        # emits a uid:N column on a (scene, phase) when that uid has
        # data there.  Intersect the requested columns with both
        # fragments' actual schemas to avoid `ArrowInvalid: No match
        # for FieldRef.Name(uid:N)`.
        names_a = set(frag_a.physical_schema.names)
        names_b = set(frag_b.physical_schema.names)
        cols_here = sorted(shared_set & names_a & names_b)
        if not cols_here:
            continue
        columns_to_read = cols_here + ["block"]
        tbl_a = frag_a.to_table(columns=columns_to_read)
        tbl_b = frag_b.to_table(columns=columns_to_read)
        # The block column is identical between the two tables (both
        # tables share the same prelude rows).
        block_arr = tbl_b["block"].to_pylist()
        dur = [block_durations.get(b if b is not None else 0, 1.0) for b in block_arr]
        dur_arr = pa.array(dur, type=pa.float64())
        for col in cols_here:
            prod = pc.multiply(tbl_a[col].cast("float64"), tbl_b[col].cast("float64"))
            weighted = pc.multiply(prod, dur_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                total += float(s)
    return total


def streaming_sol_weighted_sum(
    dataset: pads.Dataset | None,
    block_durations: dict[int, float],
    coef_per_uid: dict[int, float] | None = None,
    *,
    abs_value: bool = False,
) -> float:
    """Σ over (s,t,b,uid) of  value(s,t,b,uid) × duration(b) ×
    coef(uid).  If ``coef_per_uid`` is None, the per-uid coefficient
    is taken to be 1 (i.e. result = Σ value × duration(b)).

    When ``abs_value=True``, the per-row ``value`` is replaced by
    ``|value|`` before the weighted sum — required when the input is a
    SIGNED quantity (e.g. ``Line/flow_sol``) and the caller wants the
    magnitude-weighted total (e.g. transmission ``tcost × |flow| ×
    duration``).  Default ``False`` preserves the legacy
    sign-preserving behaviour used by every other caller.

    Streaming: never materialises the full table.  Used by the cost
    breakdown so we get the right physical answer without paying the
    pandas wide-DF memory cost.
    """
    if dataset is None:
        return 0.0
    if dataset_layout(dataset) == "long":
        total = 0.0
        for batch in _scan_batches(dataset, ["block", "uid", "value"]):
            block_arr = batch["block"].to_pylist()
            dur = [
                block_durations.get(b if b is not None else 0, 1.0) for b in block_arr
            ]
            dur_arr = pa.array(dur, type=pa.float64())
            # Per-row coefficient: if `coef_per_uid` is None, treat
            # every uid as 1.0 (pure energy / duration weighting).
            # Otherwise look up the per-uid coef in Python — the per-
            # batch length is at most a few million rows, well below
            # any cost-meaningful threshold.
            uid_arr = batch["uid"].to_pylist()
            if coef_per_uid is None:
                coef_arr = pa.array([1.0] * len(uid_arr), type=pa.float64())
            else:
                coef_arr = pa.array(
                    [
                        coef_per_uid.get(int(u), 0.0) if u is not None else 0.0
                        for u in uid_arr
                    ],
                    type=pa.float64(),
                )
            value_arr = batch["value"].cast("float64")
            if abs_value:
                value_arr = pc.abs(value_arr)
            prod = pc.multiply(value_arr, dur_arr)
            weighted = pc.multiply(prod, coef_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                total += float(s)
        return total
    cols = dataset_uid_cols(dataset)
    if not cols:
        return 0.0
    total = 0.0
    for batch in _scan_batches(dataset, cols + ["block"]):
        block_arr = batch["block"]
        # Build a per-row duration vector.
        dur = [
            block_durations.get(b if b is not None else 0, 1.0)
            for b in block_arr.to_pylist()
        ]
        dur_arr = pa.array(dur, type=pa.float64())
        for col in cols:
            uid = int(col.split(":")[1])
            coef = 1.0 if coef_per_uid is None else coef_per_uid.get(uid, 0.0)
            if coef == 0.0:
                continue
            value_arr = batch[col].cast("float64")
            if abs_value:
                value_arr = pc.abs(value_arr)
            weighted = pc.multiply(value_arr, dur_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                total += float(s) * coef
    return total


def _np_array_from_list(values: list[float]):
    """Tiny helper to build an Arrow array from a Python list without
    importing numpy at module load time (keeps the import surface tight)."""
    import numpy as np  # noqa: PLC0415

    return np.asarray(values, dtype="float64")


def streaming_sqr_weighted_sum(
    dataset: pads.Dataset | None,
    block_durations: dict[int, float],
    coef_per_uid: dict[int, float],
) -> float:
    """Σ over (s,t,b,uid) of  value(s,t,b,uid)² × duration(b) × coef(uid).

    Used by ``check_transmission_losses`` for the analytical loss
    estimate ``Σ_lines (R / V²) · flow²(s,t,b) · dur(b)`` — flow comes
    from ``Line/flow_sol``, the per-line coefficient is ``R / V²``.

    Streaming: never materialises the full table (peak memory ≈ one
    record batch, same shape as `streaming_sol_weighted_sum`).
    """
    if dataset is None:
        return 0.0
    if dataset_layout(dataset) == "long":
        total = 0.0
        for batch in _scan_batches(dataset, ["block", "uid", "value"]):
            block_arr = batch["block"].to_pylist()
            dur = [
                block_durations.get(b if b is not None else 0, 1.0) for b in block_arr
            ]
            dur_arr = pa.array(dur, type=pa.float64())
            uid_arr = batch["uid"].to_pylist()
            coef_arr = pa.array(
                [
                    coef_per_uid.get(int(u), 0.0) if u is not None else 0.0
                    for u in uid_arr
                ],
                type=pa.float64(),
            )
            value_arr = batch["value"].cast("float64")
            sqr = pc.multiply(value_arr, value_arr)
            prod = pc.multiply(sqr, dur_arr)
            weighted = pc.multiply(prod, coef_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                total += float(s)
        return total
    cols = dataset_uid_cols(dataset)
    if not cols:
        return 0.0
    total = 0.0
    for batch in _scan_batches(dataset, cols + ["block"]):
        block_arr = batch["block"]
        dur = [
            block_durations.get(b if b is not None else 0, 1.0)
            for b in block_arr.to_pylist()
        ]
        dur_arr = pa.array(dur, type=pa.float64())
        for col in cols:
            uid = int(col.split(":")[1])
            coef = coef_per_uid.get(uid, 0.0)
            if coef == 0.0:
                continue
            value_arr = batch[col].cast("float64")
            sqr = pc.multiply(value_arr, value_arr)
            weighted = pc.multiply(sqr, dur_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                total += float(s) * coef
    return total


def streaming_sqr_weighted_sum_per_uid(
    dataset: pads.Dataset | None,
    block_durations: dict[int, float],
    coef_per_uid: dict[int, float],
) -> dict[int, float]:
    """Σ over (s,t,b) per uid of  value² × duration(b) × coef(uid).
    Streaming variant of ``streaming_sqr_weighted_sum`` that keeps the
    per-uid breakdown.  Used by ``check_transmission_losses`` to rank
    lines by their analytical loss contribution and to flag potential
    loss-arbitrage candidates (lines with much larger flow than the
    net transfer they accomplish).
    """
    out: dict[int, float] = {}
    if dataset is None:
        return out
    if dataset_layout(dataset) == "long":
        for batch in _scan_batches(dataset, ["block", "uid", "value"]):
            block_arr = batch["block"].to_pylist()
            dur = [
                block_durations.get(b if b is not None else 0, 1.0) for b in block_arr
            ]
            dur_arr = pa.array(dur, type=pa.float64())
            uid_arr = batch["uid"].to_pylist()
            value_arr = batch["value"].cast("float64")
            sqr = pc.multiply(value_arr, value_arr)
            row_contribution = pc.multiply(sqr, dur_arr).to_pylist()
            for u, v in zip(uid_arr, row_contribution, strict=False):
                if u is None or v is None:
                    continue
                uid = int(u)
                coef = coef_per_uid.get(uid, 0.0)
                if coef == 0.0:
                    continue
                out[uid] = out.get(uid, 0.0) + float(v) * coef
        return out
    cols = dataset_uid_cols(dataset)
    if not cols:
        return out
    for batch in _scan_batches(dataset, cols + ["block"]):
        block_arr = batch["block"]
        dur = [
            block_durations.get(b if b is not None else 0, 1.0)
            for b in block_arr.to_pylist()
        ]
        dur_arr = pa.array(dur, type=pa.float64())
        for col in cols:
            uid = int(col.split(":")[1])
            coef = coef_per_uid.get(uid, 0.0)
            if coef == 0.0:
                continue
            value_arr = batch[col].cast("float64")
            sqr = pc.multiply(value_arr, value_arr)
            weighted = pc.multiply(sqr, dur_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                out[uid] = out.get(uid, 0.0) + float(s) * coef
    return out


def streaming_abs_weighted_sum_per_uid(
    dataset: pads.Dataset | None,
    block_durations: dict[int, float],
) -> dict[int, float]:
    """Σ over (s,t,b) per uid of  |value| × duration(b).
    Used by ``check_transmission_losses`` to compute per-line
    throughput (sum of absolute flow energy).  Combined with the
    per-line analytical loss energy this yields a loss / throughput
    ratio that flags loss-arbitrage candidates.
    """
    out: dict[int, float] = {}
    if dataset is None:
        return out
    if dataset_layout(dataset) == "long":
        for batch in _scan_batches(dataset, ["block", "uid", "value"]):
            block_arr = batch["block"].to_pylist()
            dur = [
                block_durations.get(b if b is not None else 0, 1.0) for b in block_arr
            ]
            dur_arr = pa.array(dur, type=pa.float64())
            uid_arr = batch["uid"].to_pylist()
            value_arr = batch["value"].cast("float64")
            absv = pc.abs(value_arr)
            row_contribution = pc.multiply(absv, dur_arr).to_pylist()
            for u, v in zip(uid_arr, row_contribution, strict=False):
                if u is None or v is None:
                    continue
                out[int(u)] = out.get(int(u), 0.0) + float(v)
        return out
    cols = dataset_uid_cols(dataset)
    if not cols:
        return out
    for batch in _scan_batches(dataset, cols + ["block"]):
        block_arr = batch["block"]
        dur = [
            block_durations.get(b if b is not None else 0, 1.0)
            for b in block_arr.to_pylist()
        ]
        dur_arr = pa.array(dur, type=pa.float64())
        for col in cols:
            uid = int(col.split(":")[1])
            value_arr = batch[col].cast("float64")
            absv = pc.abs(value_arr)
            weighted = pc.multiply(absv, dur_arr)
            s = pc.sum(weighted).as_py()
            if s is not None:
                out[uid] = out.get(uid, 0.0) + float(s)
    return out


def load_planning(json_path: Path) -> dict:
    """Load a planning JSON file."""
    with open(json_path, encoding="utf-8") as f:
        return json.load(f)


def get_block_durations(planning: dict) -> dict[int, float]:
    """Return {block_uid: duration_hours} from the planning dict."""
    blocks = planning.get("simulation", {}).get("block_array", [])
    result: dict[int, float] = {}
    for blk in blocks:
        uid = blk.get("uid", 0)
        dur = blk.get("duration", 1.0)
        result[uid] = dur
    return result


def get_generator_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with generator uid, name, type, bus, pmax."""
    gens = planning.get("system", {}).get("generator_array", [])
    rows = []
    for g in gens:
        uid = g.get("uid", 0)
        name = g.get("name", f"uid:{uid}")
        gtype = g.get("type", "unknown")
        bus = g.get("bus", "")
        pmax = g.get("pmax", 0)
        if isinstance(pmax, (int, float)):
            pmax_val = float(pmax)
        else:
            pmax_val = 0.0  # file-referenced
        rows.append(
            {
                "uid": uid,
                "name": name,
                "type": gtype,
                "bus": bus,
                "pmax": pmax_val,
            }
        )
    return pd.DataFrame(rows)


def get_line_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with line uid, name, bus_a, bus_b, tmax,
    resistance, voltage.  Resistance / voltage are used by
    ``check_transmission_losses`` to compute the analytical
    ``R · P² / V²`` loss estimate per line."""
    lines = planning.get("system", {}).get("line_array", [])
    rows = []
    for ln in lines:
        uid = ln.get("uid", 0)
        name = ln.get("name", f"uid:{uid}")
        bus_a = ln.get("bus_a", "")
        bus_b = ln.get("bus_b", "")
        tmax = ln.get("tmax_ab", ln.get("tmax", 0))
        if isinstance(tmax, (int, float)):
            tmax_val = float(tmax)
        else:
            tmax_val = 0.0
        resistance = ln.get("resistance", 0.0)
        voltage = ln.get("voltage", 0.0)
        rows.append(
            {
                "uid": uid,
                "name": name,
                "bus_a": bus_a,
                "bus_b": bus_b,
                "tmax": tmax_val,
                "resistance": (
                    float(resistance) if isinstance(resistance, (int, float)) else 0.0
                ),
                "voltage": (
                    float(voltage) if isinstance(voltage, (int, float)) else 0.0
                ),
            }
        )
    return pd.DataFrame(rows)


def get_generator_profile_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with generator profile uid, name, generator uid."""
    profiles = planning.get("system", {}).get("generator_profile_array", [])
    gen_info = get_generator_info(planning)
    # Build name→uid mapping for generators
    gen_name_to_uid: dict[str, int] = {}
    for _, row in gen_info.iterrows():
        gen_name_to_uid[row["name"]] = int(row["uid"])

    rows = []
    for gp in profiles:
        uid = gp.get("uid", 0)
        name = gp.get("name", f"uid:{uid}")
        gen_ref = gp.get("generator", "")
        # generator can be a uid (int) or a name (str)
        if isinstance(gen_ref, int):
            gen_uid = gen_ref
        elif isinstance(gen_ref, str) and gen_ref.isdigit():
            gen_uid = int(gen_ref)
        else:
            gen_uid = gen_name_to_uid.get(str(gen_ref), -1)
        rows.append({"uid": uid, "name": name, "generator_uid": gen_uid})
    return pd.DataFrame(rows)


def get_demand_info(planning: dict) -> pd.DataFrame:
    """Return a DataFrame with demand uid, name, bus."""
    demands = planning.get("system", {}).get("demand_array", [])
    rows = []
    for d in demands:
        uid = d.get("uid", 0)
        name = d.get("name", f"uid:{uid}")
        bus = d.get("bus", "")
        rows.append({"uid": uid, "name": name, "bus": bus})
    return pd.DataFrame(rows)
