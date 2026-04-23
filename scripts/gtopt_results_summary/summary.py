# SPDX-License-Identifier: BSD-3-Clause
"""Summary computation logic for gtopt_results_summary.

The public entry points are:

- :func:`summarize_results` — operates on a directory or ZIP file path.
- :func:`summarize_output_dict` — operates on a parsed results dict with the
  shape produced by :func:`guiservice.app._parse_results_zip`.
"""

from __future__ import annotations

import csv as _csv
import io
import os
import zipfile
from pathlib import Path
from typing import Any

import pandas as pd


# Known output file patterns (relative to the output directory) that the
# summary aggregator understands.  All other files are ignored — callers
# always get a well-defined KPI dict even when the output is incomplete.
_GEN_PATH = "Generator/generation_sol"
_GEN_COST_PATH = "Generator/generation_cost"
_LOAD_PATH = "Demand/load_sol"
_FAIL_PATH = "Demand/fail_sol"
_BAL_DUAL = "Bus/balance_dual"
_FLOW_PATH = "Line/flowp_sol"
_SOC_PATH = "Battery/vfin_sol"
_SOLUTION = "solution"

# Group labels used when breaking down totals by technology.  Since the
# C++ solver does not tag generators with a technology, the GUI Plus
# frontend may provide a per-generator-uid technology mapping via the
# optional ``tech_map`` argument.
_DEFAULT_TECH = "other"


def _sum_numeric(df: pd.DataFrame, skip_cols: set[str]) -> dict[str, float]:
    """Return column-wise sums for every numeric column not in skip_cols."""
    totals: dict[str, float] = {}
    for col in df.columns:
        if col in skip_cols:
            continue
        try:
            totals[col] = float(pd.to_numeric(df[col], errors="coerce").fillna(0).sum())
        except (TypeError, ValueError):
            continue
    return totals


def _extract_tech_total(
    per_uid_totals: dict[str, float], tech_map: dict[str, str] | None
) -> dict[str, float]:
    """Aggregate per-UID totals into per-technology totals.

    Column names are expected to follow the ``uid:<N>`` convention used by
    the gtopt output files.  When no technology map is provided, everything
    falls under ``_DEFAULT_TECH``.
    """
    by_tech: dict[str, float] = {}
    for col, val in per_uid_totals.items():
        uid_str = col.split(":", 1)[1] if col.startswith("uid:") else col
        tech = (tech_map or {}).get(uid_str, _DEFAULT_TECH)
        by_tech[tech] = by_tech.get(tech, 0.0) + val
    return by_tech


def _find_path(outputs: dict[str, Any], relpath: str) -> dict[str, Any] | None:
    """Return the outputs entry matching a parent/basename path.

    The outputs dict produced by _parse_results_zip uses keys with the
    pattern ``<parent>/<basename>``.  We look up ``Generator/generation_sol``
    directly and also under a bare ``generation_sol`` key for robustness.
    """
    if relpath in outputs:
        return outputs[relpath]
    # Fall back to just the basename
    base = relpath.split("/")[-1]
    if base in outputs:
        return outputs[base]
    return None


def _rows_to_df(entry: dict[str, Any]) -> pd.DataFrame:
    """Turn a ``{columns, data}`` entry into a pandas DataFrame."""
    cols = entry.get("columns", [])
    data = entry.get("data", [])
    if not cols or not data:
        return pd.DataFrame(columns=cols)
    return pd.DataFrame(data, columns=cols)


# ---------------------------------------------------------------------------
# Public API (dict-based)
# ---------------------------------------------------------------------------


def summarize_output_dict(
    results: dict[str, Any],
    tech_map: dict[str, str] | None = None,
    scale_objective: float = 1.0,
) -> dict[str, Any]:
    """Compute KPI summary from a parsed results dict.

    The ``results`` dict has the shape produced by
    :func:`guiservice.app._parse_results_zip`:

    - ``solution``: flat dict (e.g. ``obj_value``, ``status``)
    - ``outputs``: ``{"<parent>/<basename>": {"columns": [...], "data": [...]}}``

    Args:
        results: The parsed results dict.
        tech_map: Optional dict mapping generator UIDs (as strings) to a
            technology label.  When provided, generation is broken down by
            technology; otherwise, everything falls under ``other``.
        scale_objective: The ``options.scale_objective`` from the case JSON.
            The raw solver objective is multiplied by this value to produce a
            cost in the same units as the individual generator costs.

    Returns:
        A dict with the following keys (all values are plain Python types):

        - ``status`` (int | str | None) — solver status from ``solution.csv``
        - ``obj_value`` (float | None) — total cost (scaled)
        - ``obj_value_raw`` (float | None) — raw solver objective
        - ``total_generation`` (float) — sum of Generator/generation_sol
        - ``generation_by_tech`` (dict[str, float])
        - ``total_cost_generation`` (float) — sum of Generator/generation_cost
        - ``total_load`` (float) — sum of Demand/load_sol
        - ``total_unserved`` (float) — sum of Demand/fail_sol
        - ``peak_unserved`` (float) — max single-block unserved load
        - ``n_generators`` (int)
        - ``n_buses`` (int)
        - ``n_lines`` (int)
        - ``lmp_min`` / ``lmp_max`` / ``lmp_mean`` (float | None) — from Bus/balance_dual
        - ``n_blocks`` (int) — number of (scenario, stage, block) rows in Generator/generation_sol
    """
    outputs = results.get("outputs", {})
    solution = results.get("solution", {})

    summary: dict[str, Any] = {
        "status": solution.get("status"),
        "obj_value": None,
        "obj_value_raw": None,
        "total_generation": 0.0,
        "generation_by_tech": {},
        "total_cost_generation": 0.0,
        "total_load": 0.0,
        "total_unserved": 0.0,
        "peak_unserved": 0.0,
        "n_generators": 0,
        "n_buses": 0,
        "n_lines": 0,
        "lmp_min": None,
        "lmp_max": None,
        "lmp_mean": None,
        "n_blocks": 0,
    }

    try:
        raw_obj = solution.get("obj_value")
        if raw_obj is not None:
            summary["obj_value_raw"] = float(raw_obj)
            summary["obj_value"] = float(raw_obj) * float(scale_objective)
    except (TypeError, ValueError):
        pass

    # Indexing columns present in every per-block CSV.
    skip_cols = {"scenario", "stage", "block", "phase", "scene"}

    # Generation
    gen = _find_path(outputs, _GEN_PATH)
    if gen is not None:
        df = _rows_to_df(gen)
        totals = _sum_numeric(df, skip_cols)
        summary["total_generation"] = float(sum(totals.values()))
        summary["generation_by_tech"] = _extract_tech_total(totals, tech_map)
        summary["n_generators"] = len(totals)
        summary["n_blocks"] = len(df)

    # Generation cost
    gen_cost = _find_path(outputs, _GEN_COST_PATH)
    if gen_cost is not None:
        df = _rows_to_df(gen_cost)
        summary["total_cost_generation"] = float(
            sum(_sum_numeric(df, skip_cols).values())
        )

    # Load
    load = _find_path(outputs, _LOAD_PATH)
    if load is not None:
        df = _rows_to_df(load)
        summary["total_load"] = float(sum(_sum_numeric(df, skip_cols).values()))

    # Unserved demand
    fail = _find_path(outputs, _FAIL_PATH)
    if fail is not None:
        df = _rows_to_df(fail)
        per_col = _sum_numeric(df, skip_cols)
        summary["total_unserved"] = float(sum(per_col.values()))
        # Peak single-cell unserved across all cells
        peak = 0.0
        for col in df.columns:
            if col in skip_cols:
                continue
            try:
                col_max = float(pd.to_numeric(df[col], errors="coerce").max())
                peak = max(peak, col_max)
            except (TypeError, ValueError):
                continue
        summary["peak_unserved"] = peak

    # LMPs from balance_dual
    bal = _find_path(outputs, _BAL_DUAL)
    if bal is not None:
        df = _rows_to_df(bal)
        mins: list[float] = []
        maxs: list[float] = []
        means: list[float] = []
        for col in df.columns:
            if col in skip_cols:
                continue
            vals = pd.to_numeric(df[col], errors="coerce").dropna()
            if vals.empty:
                continue
            mins.append(float(vals.min()))
            maxs.append(float(vals.max()))
            means.append(float(vals.mean()))
        if mins:
            summary["lmp_min"] = min(mins)
            summary["lmp_max"] = max(maxs)
            summary["lmp_mean"] = sum(means) / len(means)
        # Number of buses = column count minus indexing columns
        summary["n_buses"] = len([c for c in df.columns if c not in skip_cols])

    # Line count (from flowp_sol) if available
    flow = _find_path(outputs, _FLOW_PATH)
    if flow is not None:
        df = _rows_to_df(flow)
        summary["n_lines"] = len([c for c in df.columns if c not in skip_cols])

    return summary


# ---------------------------------------------------------------------------
# Public API (filesystem-based)
# ---------------------------------------------------------------------------


def _read_output_dir(path: Path) -> dict[str, Any]:
    """Walk an output directory and build a dict mimicking _parse_results_zip."""
    results: dict[str, Any] = {"solution": {}, "outputs": {}, "terminal_output": ""}
    for root, _, files in os.walk(path):
        parent = os.path.basename(root) if Path(root) != path else ""
        for fname in files:
            full = Path(root) / fname
            name = fname.lower()
            try:
                if name == "solution.csv":
                    with open(full, "r", encoding="utf-8") as fh:
                        for row in _csv.reader(fh):
                            if len(row) >= 2:
                                results["solution"][row[0]] = row[1]
                elif name.endswith(".parquet"):
                    df = pd.read_parquet(full)
                    base = fname[: -len(".parquet")]
                    key = f"{parent}/{base}" if parent else base
                    results["outputs"][key] = {
                        "columns": list(df.columns),
                        "data": df.values.tolist(),
                    }
                elif name.endswith(".csv"):
                    df = pd.read_csv(full)
                    base = fname[: -len(".csv")]
                    key = f"{parent}/{base}" if parent else base
                    results["outputs"][key] = {
                        "columns": list(df.columns),
                        "data": df.values.tolist(),
                    }
            except (OSError, ValueError, pd.errors.ParserError):
                continue
    return results


def _read_zip(path: Path) -> dict[str, Any]:
    """Parse a results ZIP into the same dict shape as _parse_results_zip."""
    # Defer the import to avoid creating a circular dependency at import time.
    try:
        # pylint: disable=import-outside-toplevel
        from guiservice.app import _parse_results_zip
    except ImportError:
        _parse_results_zip = None

    if _parse_results_zip is not None:
        with open(path, "rb") as fh:
            return _parse_results_zip(fh.read())

    # Fallback: do the parse inline
    results: dict[str, Any] = {"solution": {}, "outputs": {}, "terminal_output": ""}
    with zipfile.ZipFile(path, "r") as zf:
        for name in zf.namelist():
            base = os.path.basename(name)
            parent = os.path.basename(os.path.dirname(name))
            try:
                if base == "solution.csv":
                    content = zf.read(name).decode("utf-8")
                    for row in _csv.reader(io.StringIO(content)):
                        if len(row) >= 2:
                            results["solution"][row[0]] = row[1]
                elif name.endswith(".csv"):
                    df = pd.read_csv(io.BytesIO(zf.read(name)))
                    no_ext = base[: -len(".csv")]
                    key = f"{parent}/{no_ext}" if parent else no_ext
                    results["outputs"][key] = {
                        "columns": list(df.columns),
                        "data": df.values.tolist(),
                    }
                elif name.endswith(".parquet"):
                    df = pd.read_parquet(io.BytesIO(zf.read(name)))
                    no_ext = base[: -len(".parquet")]
                    key = f"{parent}/{no_ext}" if parent else no_ext
                    results["outputs"][key] = {
                        "columns": list(df.columns),
                        "data": df.values.tolist(),
                    }
            except (OSError, ValueError, pd.errors.ParserError):
                continue
    return results


def summarize_results(
    source: str | os.PathLike[str],
    tech_map: dict[str, str] | None = None,
    scale_objective: float = 1.0,
) -> dict[str, Any]:
    """Summarize results from a directory or ZIP file path.

    Args:
        source: Path to a directory or ZIP file containing gtopt output files.
        tech_map: Optional per-UID technology mapping (see
            :func:`summarize_output_dict`).
        scale_objective: The ``options.scale_objective`` value used to unscale
            the objective value.

    Returns:
        A KPI summary dict (see :func:`summarize_output_dict`).
    """
    path = Path(source)
    if not path.exists():
        raise FileNotFoundError(f"No such file or directory: {path}")
    if path.is_dir():
        results = _read_output_dir(path)
    elif path.suffix.lower() == ".zip":
        results = _read_zip(path)
    else:
        raise ValueError(f"Expected a directory or .zip file, got {path}")
    return summarize_output_dict(
        results, tech_map=tech_map, scale_objective=scale_objective
    )
