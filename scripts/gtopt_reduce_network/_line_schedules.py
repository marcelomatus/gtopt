# SPDX-License-Identifier: BSD-3-Clause
"""Aggregate per-line parquet schedules under the corridor reduction.

When a case schedules a Line field over time (e.g. ``Line/tmax_ab.parquet``
for capacity derating, ``Line/active.parquet`` for maintenance), the
reducer needs to fold those per-original schedules into per-corridor
schedules, written as **new parquet files alongside the originals**.

Naming convention (per user request 2026-05-14): the original parquet
``Line/<field>.parquet`` is **untouched**, and the reducer emits a new
sibling ``Line/<field>_<tag>.parquet`` with corridor-equivalent
``uid:<eq_uid>`` columns.  The reduced JSON's per-line field value is
set to the new stem (e.g. ``"tmax_ab_red40"``) so gtopt's loader
resolves to the right file.

v1 scope (locked):
  * Aggregated: ``active`` (OR), ``tmax_ab`` / ``tmax_ba`` (gated sum),
    ``voltage`` (capacity-weighted active), ``reactance`` (parallel
    in per-unit), ``resistance`` (loss-energy preserving in per-unit).
  * Unhandled (warned and skipped — reduced JSON keeps scalar):
    ``tcost``, ``lossfactor``, ``tap_ratio``, ``phase_shift_deg``,
    expansion fields, bool flags.
  * Unknown fields (no rule defined): warned and skipped.

Per-unit pipeline (for V/X/R, gated by ``active``):

    X_pu[i](s)  = X[i](s) · MVA_base / V[i](s)²
    V_eq(s)     = Σ act·cap·V / Σ act·cap         (cap-weighted active)
    X_pu_eq(s)  = 1 / Σ (act[i] / X_pu[i](s))     (parallel over active)
    R_pu_eq(s)  = X_pu_eq(s)² · Σ act·R_pu / X_pu² (loss-energy)
    X_eq(s)     = X_pu_eq(s) · V_eq(s)² / MVA_base
    R_eq(s)     = R_pu_eq(s) · V_eq(s)² / MVA_base

When no V is scheduled and JSON voltage is 1.0 (pure p.u. mode), the
pipeline degenerates to the simple parallel-X / loss-energy aggregator
without any V-conversion (strict generalisation, no regression).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
import pyarrow.parquet as pq

from gtopt_reduce_network._aggregate import AggregatedLine

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Rule registry
# ---------------------------------------------------------------------------

MVA_BASE = 100.0

# Default V when neither JSON nor parquet specifies one — interpreted as
# pure-per-unit mode (X, R already in p.u.).  In that mode the per-unit
# pipeline collapses to the simple parallel-X math.
_DEFAULT_V = 1.0

# axis: "stage" (rows indexed by stage) or "stage-block" (rows indexed
# by (stage, block) tuple).  kind: dispatch key for _apply_rule.
_RULES: dict[str, dict[str, str]] = {
    "active": {"axis": "stage", "kind": "or"},
    "voltage": {"axis": "stage", "kind": "cap-weighted-active"},
    "reactance": {"axis": "stage", "kind": "parallel-x-pu"},
    "resistance": {"axis": "stage", "kind": "r-pu"},
    "tmax_ab": {"axis": "stage-block", "kind": "gated-sum"},
    "tmax_ba": {"axis": "stage-block", "kind": "gated-sum"},
}

# Topological order so dependent rules (reactance, resistance) run after
# their inputs (active, voltage).
_TOPO_ORDER: tuple[str, ...] = (
    "active",
    "voltage",
    "reactance",
    "resistance",
    "tmax_ab",
    "tmax_ba",
)

# Schedulable Line fields the v1 reducer recognises but does not aggregate.
# Warn-and-skip so unmodelled time variation is at least visible.
_UNHANDLED_FIELDS: frozenset[str] = frozenset(
    {
        "tcost",
        "lossfactor",
        "tap_ratio",
        "phase_shift_deg",
        "capacity",
        "expcap",
        "expmod",
        "capmax",
        "annual_capcost",
        "annual_derating",
        "use_line_losses",
        "integer_expmod",
    }
)


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------


@dataclass(slots=True)
class _ScheduleTable:
    """Wraps a per-line parquet schedule with scalar-fallback semantics."""

    field: str
    axis: str  # "stage" or "stage-block"
    df: pd.DataFrame  # original frame as read from parquet
    uid_cols: list[int]  # uids that have parquet overrides
    stage_col: str | None  # "stage" if axis includes stage
    block_col: str | None  # "block" if axis = "stage-block"

    @classmethod
    def from_parquet(cls, field: str, axis: str, path: Path) -> _ScheduleTable:
        df = pq.read_table(path).to_pandas()
        # gtopt's I/O is long-only: schedule files arrive as
        # ``[<index cols...>, uid, value]``.  Pivot to the wide ``uid:<N>``
        # shape this reducer operates on (older fixtures shipped wide and
        # skip this branch).  Index cols are everything but ``uid``/``value``.
        if "uid" in df.columns and "value" in df.columns:
            index_cols = [c for c in df.columns if c not in ("uid", "value")]
            df = df.pivot_table(
                index=index_cols, columns="uid", values="value", aggfunc="first"
            ).reset_index()
            df.columns = [
                col if isinstance(col, str) else f"uid:{int(col)}" for col in df.columns
            ]
            df.columns.name = None
        uids: list[int] = []
        for col in df.columns:
            if col.startswith("uid:"):
                try:
                    uids.append(int(col[4:]))
                except ValueError:
                    continue
        stage_col = "stage" if "stage" in df.columns else None
        block_col = "block" if "block" in df.columns else None
        return cls(
            field=field,
            axis=axis,
            df=df,
            uid_cols=sorted(uids),
            stage_col=stage_col,
            block_col=block_col,
        )

    def stages(self) -> np.ndarray:
        if self.stage_col is None:
            return np.asarray([], dtype=np.int64)
        return self.df[self.stage_col].to_numpy()

    def value_for(self, uid: int) -> np.ndarray | None:
        """Return the per-row series for ``uid:<uid>`` or None if absent."""
        col = f"uid:{uid}"
        if col in self.df.columns:
            return self.df[col].to_numpy()
        return None


@dataclass(slots=True)
class _ScalarsFor:
    """Per-line scalar fallback values from the JSON line_array."""

    active: float
    voltage: float
    reactance: float
    resistance: float
    tmax_ab: float
    tmax_ba: float
    capacity: float  # used as weight in V_eq; falls back to tmax_ab


@dataclass(slots=True)
class _AggResult:
    field: str
    df: pd.DataFrame
    output_stem: str


def _scalars_from_line_array(
    line_array: list[dict[str, Any]],
) -> dict[int, _ScalarsFor]:
    out: dict[int, _ScalarsFor] = {}
    for ln in line_array:
        if "uid" not in ln:
            continue
        uid = int(ln["uid"])
        v_active = _coerce_bool_or_int(ln.get("active"), default=1)
        v_volt = _coerce_float(ln.get("voltage"), default=_DEFAULT_V)
        v_react = _coerce_float(ln.get("reactance"), default=1.0)
        v_res = _coerce_float(ln.get("resistance"), default=0.0)
        v_tab = _coerce_float(ln.get("tmax_ab"), default=0.0)
        v_tba = _coerce_float(ln.get("tmax_ba"), default=0.0)
        cap = _coerce_float(ln.get("capacity"), default=v_tab) or v_tab
        out[uid] = _ScalarsFor(
            active=float(v_active),
            voltage=float(v_volt) if v_volt > 0 else _DEFAULT_V,
            reactance=float(v_react) if v_react > 0 else 1.0,
            resistance=float(v_res),
            tmax_ab=float(v_tab),
            tmax_ba=float(v_tba),
            capacity=float(cap) if cap > 0 else 1.0,
        )
    return out


def _coerce_bool_or_int(v: Any, *, default: int) -> int:
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)):
        return int(v)
    return default


def _coerce_float(v: Any, *, default: float) -> float:
    if isinstance(v, bool):
        return default
    if isinstance(v, (int, float)):
        return float(v)
    return default


# ---------------------------------------------------------------------------
# Per-rule aggregators
# ---------------------------------------------------------------------------


def _resolve_series_stage(
    table: _ScheduleTable | None,
    uid: int,
    scalar: float,
    n_stages: int,
) -> np.ndarray:
    """Return a length-``n_stages`` array for the given uid: parquet
    column if present, otherwise the scalar broadcast to all stages."""
    if table is not None:
        v = table.value_for(uid)
        if v is not None:
            return np.asarray(v, dtype=float)
    return np.full(n_stages, float(scalar), dtype=float)


def _build_stage_axis(tables: dict[str, _ScheduleTable]) -> np.ndarray:
    """Canonical stage list: unique sorted stages from any table that
    carries a ``stage`` column (stage-only OR stage-block)."""
    seen: set[int] = set()
    for t in tables.values():
        if t.stage_col is None:
            continue
        for s in t.df[t.stage_col].to_numpy():
            seen.add(int(s))
    return np.asarray(sorted(seen), dtype=np.int64)


def _build_stage_block_axis(
    tables: dict[str, _ScheduleTable],
) -> pd.DataFrame:
    """Return a DataFrame with [stage, block] columns derived from any
    stage-block scheduled field (the union should match in practice)."""
    for t in tables.values():
        if t.axis == "stage-block" and t.stage_col and t.block_col:
            return t.df[[t.stage_col, t.block_col]].copy()
    return pd.DataFrame({"stage": [], "block": []})


def aggregate_line_schedules(
    case_dir: Path,
    input_directory: str,
    reduced_tag: str,
    aggregated_lines: list[AggregatedLine],
    original_line_array: list[dict[str, Any]],
) -> dict[str, str]:
    """Discover ``Line/*.parquet`` schedules in the original case,
    aggregate per corridor, and write ``Line/<field>_<tag>.parquet``
    sibling files.

    Returns a ``{field_name: new_stem}`` map so the caller can rewrite
    the reduced JSON's per-line field references.  Empty dict if
    nothing was aggregated.
    """
    if not reduced_tag:
        return {}
    line_dir = (case_dir / input_directory / "Line").resolve()
    if not line_dir.is_dir():
        logger.info("no Line/ subdir under '%s'; no schedules to aggregate", line_dir)
        return {}

    # 1. Discover parquet files.
    discovered: dict[str, _ScheduleTable] = {}
    skipped_unhandled: list[str] = []
    skipped_unknown: list[str] = []
    suffix = f"_{reduced_tag}"
    for p in sorted(line_dir.glob("*.parquet")):
        stem = p.stem
        if stem.endswith(suffix):
            continue  # re-run safety
        if stem in _UNHANDLED_FIELDS:
            skipped_unhandled.append(stem)
            continue
        if stem not in _RULES:
            skipped_unknown.append(stem)
            continue
        discovered[stem] = _ScheduleTable.from_parquet(
            field=stem, axis=_RULES[stem]["axis"], path=p
        )
    if skipped_unhandled:
        logger.warning(
            "Line schedules not aggregated in v1 (reduced JSON uses scalar): %s",
            sorted(skipped_unhandled),
        )
    if skipped_unknown:
        logger.warning(
            "Line schedules with no aggregation rule: %s — skipped",
            sorted(skipped_unknown),
        )
    if not discovered:
        return {}

    # 2. Build per-line scalar fallback table.
    scalars = _scalars_from_line_array(original_line_array)
    stage_axis = _build_stage_axis(discovered)
    n_stages = len(stage_axis)

    # Topologically aggregate.
    computed: dict[str, np.ndarray] = {}  # field -> shape (n_eq, n_stages)
    eq_uids = [eq.uid for eq in aggregated_lines]
    n_eq = len(eq_uids)
    if n_eq == 0:
        return {}

    # Build per-eq active(s) matrix (used by every later rule).
    if "active" in discovered:
        act_mat = _build_per_orig_matrix(
            discovered["active"],
            scalars,
            "active",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        eq_active = _eq_or(act_mat)
        computed["active"] = eq_active
    else:
        # No active.parquet → every original active everywhere.
        act_mat_per_eq: list[np.ndarray] = []
        for eq in aggregated_lines:
            n_orig = len(eq.absorbed)
            mat = np.ones((n_orig, n_stages), dtype=float)
            act_mat_per_eq.append(mat)
        # Use the same shape but only filled with 1's.
        eq_active = np.ones((n_eq, n_stages), dtype=int)
        computed["active"] = eq_active

    # Voltage (needed by reactance/resistance pipelines).
    if "voltage" in discovered:
        V_mat = _per_orig_matrix_for(
            discovered.get("voltage"),
            scalars,
            "voltage",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        # We also need the act mat with the SAME (n_orig, n_stages) shape.
        act_per_orig = _per_orig_matrix_for(
            discovered.get("active"),
            scalars,
            "active",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        eq_voltage = _eq_cap_weighted_active(
            V_mat,
            act_per_orig,
            aggregated_lines,
            scalars,
        )
        computed["voltage"] = eq_voltage
    else:
        eq_voltage = None

    # Reactance.
    if "reactance" in discovered:
        X_mat = _per_orig_matrix_for(
            discovered.get("reactance"),
            scalars,
            "reactance",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        V_mat_for_x = _per_orig_matrix_for(
            discovered.get("voltage"),
            scalars,
            "voltage",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        act_per_orig_x = _per_orig_matrix_for(
            discovered.get("active"),
            scalars,
            "active",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        V_eq_per_stage = computed.get("voltage")
        if V_eq_per_stage is None:
            # Voltage not scheduled — derive V_eq from JSON scalars per eq.
            V_eq_per_stage = _eq_cap_weighted_active(
                V_mat_for_x,
                act_per_orig_x,
                aggregated_lines,
                scalars,
            )
        eq_reactance = _eq_parallel_x_pu(
            X_mat,
            V_mat_for_x,
            act_per_orig_x,
            V_eq_per_stage,
        )
        computed["reactance"] = eq_reactance
    else:
        eq_reactance = None

    # Resistance.
    if "resistance" in discovered:
        R_mat = _per_orig_matrix_for(
            discovered.get("resistance"),
            scalars,
            "resistance",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        X_mat_for_r = _per_orig_matrix_for(
            discovered.get("reactance"),
            scalars,
            "reactance",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        V_mat_for_r = _per_orig_matrix_for(
            discovered.get("voltage"),
            scalars,
            "voltage",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        act_per_orig_r = _per_orig_matrix_for(
            discovered.get("active"),
            scalars,
            "active",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        V_eq_per_stage = computed.get("voltage")
        if V_eq_per_stage is None:
            V_eq_per_stage = _eq_cap_weighted_active(
                V_mat_for_r,
                act_per_orig_r,
                aggregated_lines,
                scalars,
            )
        X_eq_per_stage = computed.get("reactance")
        if X_eq_per_stage is None:
            X_eq_per_stage = _eq_parallel_x_pu(
                X_mat_for_r,
                V_mat_for_r,
                act_per_orig_r,
                V_eq_per_stage,
            )
        eq_resistance = _eq_resistance_pu(
            R_mat,
            X_mat_for_r,
            V_mat_for_r,
            act_per_orig_r,
            V_eq_per_stage,
            X_eq_per_stage,
        )
        computed["resistance"] = eq_resistance
    else:
        eq_resistance = None

    # 5. Stage-indexed outputs.
    results: dict[str, _AggResult] = {}
    for field in ("active", "voltage", "reactance", "resistance"):
        if field not in computed or field not in discovered:
            continue
        mat = computed[field]  # shape (n_eq, n_stages)
        out_stem = f"{field}_{reduced_tag}"
        df = _stage_df_from_matrix(
            stage_axis, eq_uids, mat, dtype_int=(field == "active")
        )
        results[field] = _AggResult(field=field, df=df, output_stem=out_stem)

    # 6. Stage-block outputs (tmax_ab, tmax_ba).
    for field in ("tmax_ab", "tmax_ba"):
        if field not in discovered:
            continue
        table = discovered[field]
        # Per-orig values per (stage, block) row.
        per_orig_sb = _per_orig_matrix_sb(
            table,
            scalars,
            field,
            aggregated_lines,
        )
        # Per-orig active values broadcast to per-block (active is stage-only
        # → broadcast across blocks within each stage).
        act_per_orig = _per_orig_matrix_for(
            discovered.get("active"),
            scalars,
            "active",
            stage_axis,
            n_stages,
            aggregated_lines,
        )
        sb_active = _broadcast_stage_to_sb(
            act_per_orig,
            table.df[["stage"]].to_numpy().flatten(),
            stage_axis,
        )
        # Aggregate: gated sum.
        eq_sb = _eq_gated_sum_sb(per_orig_sb, sb_active)
        out_stem = f"{field}_{reduced_tag}"
        df = _stage_block_df_from_matrix(
            table.df,
            eq_uids,
            eq_sb,
        )
        results[field] = _AggResult(field=field, df=df, output_stem=out_stem)

    # 7. Write parquets to disk + return field→stem map.
    out_map: dict[str, str] = {}
    for field, res in results.items():
        out_path = line_dir / f"{res.output_stem}.parquet"
        res.df.to_parquet(out_path, index=False, compression="snappy")
        n_corr = sum(1 for c in res.df.columns if c.startswith("uid:"))
        logger.info(
            "line-schedule: wrote %s (%d rows, %d corridor cols)",
            out_path.name,
            len(res.df),
            n_corr,
        )
        out_map[field] = res.output_stem
    return out_map


# ---------------------------------------------------------------------------
# Per-original matrix builders
# ---------------------------------------------------------------------------


def _per_orig_matrix_for(
    table: _ScheduleTable | None,
    scalars: dict[int, _ScalarsFor],
    field: str,
    stage_axis: np.ndarray,
    n_stages: int,
    aggregated_lines: list[AggregatedLine],
) -> list[np.ndarray]:
    """For each eq line, return its (n_orig, n_stages) matrix."""
    mats: list[np.ndarray] = []
    for eq in aggregated_lines:
        orig_uids = eq.absorbed
        rows: list[np.ndarray] = []
        for u in orig_uids:
            sc = scalars.get(u)
            scalar_v = float(getattr(sc, field, 0.0)) if sc is not None else 0.0
            rows.append(_resolve_series_stage(table, u, scalar_v, n_stages))
        if rows:
            mats.append(np.vstack(rows))
        else:
            mats.append(np.zeros((0, n_stages)))
    return mats


def _build_per_orig_matrix(
    table: _ScheduleTable | None,
    scalars: dict[int, _ScalarsFor],
    field: str,
    stage_axis: np.ndarray,
    n_stages: int,
    aggregated_lines: list[AggregatedLine],
) -> list[np.ndarray]:
    return _per_orig_matrix_for(
        table, scalars, field, stage_axis, n_stages, aggregated_lines
    )


def _per_orig_matrix_sb(
    table: _ScheduleTable,
    scalars: dict[int, _ScalarsFor],
    field: str,
    aggregated_lines: list[AggregatedLine],
) -> list[np.ndarray]:
    """For each eq line, return its (n_orig, n_rows) matrix where
    n_rows is the number of (stage, block) rows in ``table.df``."""
    n_rows = len(table.df)
    mats: list[np.ndarray] = []
    for eq in aggregated_lines:
        rows: list[np.ndarray] = []
        for u in eq.absorbed:
            col = f"uid:{u}"
            if col in table.df.columns:
                rows.append(table.df[col].to_numpy(dtype=float))
            else:
                sc = scalars.get(u)
                scalar_v = float(getattr(sc, field, 0.0)) if sc is not None else 0.0
                rows.append(np.full(n_rows, scalar_v, dtype=float))
        if rows:
            mats.append(np.vstack(rows))
        else:
            mats.append(np.zeros((0, n_rows)))
    return mats


def _broadcast_stage_to_sb(
    stage_mat_per_eq: list[np.ndarray],
    sb_stage_col: np.ndarray,
    stage_axis: np.ndarray,
) -> list[np.ndarray]:
    """Broadcast a (n_orig, n_stages) matrix to (n_orig, n_sb_rows) by
    looking up each (stage, block) row's stage index in stage_axis."""
    stage_to_idx = {int(s): i for i, s in enumerate(stage_axis)}
    sb_idx = np.asarray(
        [stage_to_idx.get(int(s), -1) for s in sb_stage_col], dtype=np.int64
    )
    out: list[np.ndarray] = []
    for mat in stage_mat_per_eq:
        if mat.shape[0] == 0:
            out.append(np.zeros((0, len(sb_idx)), dtype=mat.dtype))
            continue
        # Replace -1 (unknown stage) with 0 column for safety; the active
        # values there will be JSON-scalar broadcasts, which are 1 by default.
        safe_idx = np.where(sb_idx >= 0, sb_idx, 0)
        out.append(mat[:, safe_idx])
    return out


# ---------------------------------------------------------------------------
# Per-rule reducers
# ---------------------------------------------------------------------------


def _eq_or(act_per_eq: list[np.ndarray]) -> np.ndarray:
    """OR across originals: corridor active iff any original active."""
    n_eq = len(act_per_eq)
    n_stages = act_per_eq[0].shape[1] if (act_per_eq and act_per_eq[0].size) else 0
    out = np.zeros((n_eq, n_stages), dtype=int)
    for i, mat in enumerate(act_per_eq):
        if mat.size == 0:
            continue
        out[i] = (mat.sum(axis=0) > 0).astype(int)
    return out


def _eq_cap_weighted_active(
    V_per_eq: list[np.ndarray],
    act_per_eq: list[np.ndarray],
    aggregated_lines: list[AggregatedLine],
    scalars: dict[int, _ScalarsFor],
) -> np.ndarray:
    """V_eq(s) = Σ act·cap·V / Σ act·cap (capacity-weighted active)."""
    n_eq = len(V_per_eq)
    n_stages = V_per_eq[0].shape[1] if (V_per_eq and V_per_eq[0].size) else 0
    out = np.full((n_eq, n_stages), _DEFAULT_V, dtype=float)
    for i, (V_mat, act_mat, eq) in enumerate(
        zip(V_per_eq, act_per_eq, aggregated_lines)
    ):
        if V_mat.size == 0 or act_mat.size == 0:
            continue
        cap = np.asarray(
            [
                scalars.get(u, _ScalarsFor(0, 1.0, 1.0, 0, 0, 0, 1.0)).capacity
                for u in eq.absorbed
            ],
            dtype=float,
        )[:, None]
        weight = act_mat * cap
        num = (V_mat * weight).sum(axis=0)
        den = weight.sum(axis=0)
        v_eq = np.where(den > 0, num / np.where(den > 0, den, 1.0), _DEFAULT_V)
        # Warn if V varies meaningfully across active originals.
        max_v = (V_mat * (act_mat > 0)).max(axis=0)
        min_v = np.where(act_mat > 0, V_mat, np.inf).min(axis=0)
        bad = np.any(
            np.where(min_v > 0, max_v / np.where(min_v > 0, min_v, 1.0), 1.0) > 1.05
        )
        if bad:
            logger.warning(
                "line-schedule: corridor uid=%d has mixed-voltage active "
                "originals (max/min > 1.05) — V_eq is cap-weighted",
                eq.uid,
            )
        out[i] = v_eq
    return out


def _eq_parallel_x_pu(
    X_per_eq: list[np.ndarray],
    V_per_eq: list[np.ndarray],
    act_per_eq: list[np.ndarray],
    V_eq: np.ndarray,
) -> np.ndarray:
    """X_eq(s) = X_pu_eq(s) · V_eq(s)² / MVA_base where
    X_pu_eq(s) = 1 / Σ(act[i] / X_pu[i](s))  over active originals."""
    n_eq = len(X_per_eq)
    n_stages = V_eq.shape[1] if V_eq.ndim == 2 else 0
    out = np.zeros((n_eq, n_stages), dtype=float)
    for i in range(n_eq):
        X_mat = X_per_eq[i]
        V_mat = V_per_eq[i]
        act_mat = act_per_eq[i]
        if X_mat.size == 0:
            continue
        # Per-unit per-original X.
        V_safe = np.where(V_mat > 0, V_mat, _DEFAULT_V)
        X_safe = np.where(X_mat > 0, X_mat, 1.0)
        X_pu = X_safe * MVA_BASE / (V_safe**2)
        # Inverse-sum over active subset.
        inv_X_pu = np.where(act_mat > 0, 1.0 / X_pu, 0.0)
        denom = inv_X_pu.sum(axis=0)
        X_pu_eq = np.where(denom > 0, 1.0 / np.where(denom > 0, denom, 1.0), 1.0)
        # Convert back using V_eq.
        V_eq_safe = np.where(V_eq[i] > 0, V_eq[i], _DEFAULT_V)
        out[i] = X_pu_eq * (V_eq_safe**2) / MVA_BASE
    return out


def _eq_resistance_pu(
    R_per_eq: list[np.ndarray],
    X_per_eq: list[np.ndarray],
    V_per_eq: list[np.ndarray],
    act_per_eq: list[np.ndarray],
    V_eq: np.ndarray,
    X_eq: np.ndarray,
) -> np.ndarray:
    """R_eq(s) = X_pu_eq² · Σ act·R_pu / X_pu² · V_eq² / MVA_base."""
    n_eq = len(R_per_eq)
    n_stages = V_eq.shape[1] if V_eq.ndim == 2 else 0
    out = np.zeros((n_eq, n_stages), dtype=float)
    for i in range(n_eq):
        R_mat = R_per_eq[i]
        X_mat = X_per_eq[i]
        V_mat = V_per_eq[i]
        act_mat = act_per_eq[i]
        if R_mat.size == 0:
            continue
        V_safe = np.where(V_mat > 0, V_mat, _DEFAULT_V)
        X_safe = np.where(X_mat > 0, X_mat, 1.0)
        X_pu = X_safe * MVA_BASE / (V_safe**2)
        R_pu = R_mat * MVA_BASE / (V_safe**2)
        # Compute X_pu_eq from same active mask (parallel).
        inv_X_pu = np.where(act_mat > 0, 1.0 / X_pu, 0.0)
        denom = inv_X_pu.sum(axis=0)
        X_pu_eq = np.where(denom > 0, 1.0 / np.where(denom > 0, denom, 1.0), 1.0)
        # R_pu_eq = X_pu_eq² · Σ act · R_pu / X_pu².
        weighted = np.where(act_mat > 0, R_pu / (X_pu**2), 0.0)
        R_pu_eq = (X_pu_eq**2) * weighted.sum(axis=0)
        V_eq_safe = np.where(V_eq[i] > 0, V_eq[i], _DEFAULT_V)
        out[i] = R_pu_eq * (V_eq_safe**2) / MVA_BASE
    return out


def _eq_gated_sum_sb(
    per_orig_sb: list[np.ndarray],
    act_per_orig_sb: list[np.ndarray],
) -> np.ndarray:
    """Gated sum across originals at each (stage, block) tick."""
    n_eq = len(per_orig_sb)
    n_rows = per_orig_sb[0].shape[1] if (per_orig_sb and per_orig_sb[0].size) else 0
    out = np.zeros((n_eq, n_rows), dtype=float)
    for i, (vals, act) in enumerate(zip(per_orig_sb, act_per_orig_sb)):
        if vals.size == 0:
            continue
        gated = vals * (act > 0).astype(float)
        out[i] = gated.sum(axis=0)
    return out


# ---------------------------------------------------------------------------
# Output frame builders
# ---------------------------------------------------------------------------


def _stage_df_from_matrix(
    stage_axis: np.ndarray,
    eq_uids: list[int],
    mat: np.ndarray,
    *,
    dtype_int: bool = False,
) -> pd.DataFrame:
    cols: dict[str, Any] = {"stage": np.asarray(stage_axis, dtype=np.int32)}
    for j, u in enumerate(eq_uids):
        col = mat[j]
        if dtype_int:
            cols[f"uid:{u}"] = col.astype(np.int32)
        else:
            cols[f"uid:{u}"] = col.astype(np.float64)
    return pd.DataFrame(cols)


def _stage_block_df_from_matrix(
    src_df: pd.DataFrame,
    eq_uids: list[int],
    mat: np.ndarray,
) -> pd.DataFrame:
    cols: dict[str, Any] = {}
    if "block" in src_df.columns:
        cols["block"] = src_df["block"].to_numpy(dtype=np.int32)
    for j, u in enumerate(eq_uids):
        cols[f"uid:{u}"] = mat[j].astype(np.float64)
    if "stage" in src_df.columns:
        cols["stage"] = src_df["stage"].to_numpy(dtype=np.int32)
    return pd.DataFrame(cols)
