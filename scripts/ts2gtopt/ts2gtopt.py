"""ts2gtopt – Project a time-series onto a gtopt planning horizon.

Given an hourly (or finer) time-series and a planning horizon definition
(scenarios, stages, blocks), this module aggregates the raw data into
representative block values suitable for use as gtopt schedule files.

**Conservation guarantees**

1. *Period conservation* – the sum of all effective block durations equals the
   total time spanned by the input data::

       Σ_{s,b} block_duration[s,b]  =  total_period

2. *Energy conservation* – the sum of ``value × block_duration`` over the
   projected schedule equals the sum of ``value × interval`` over the original
   time-series::

       Σ_{s,b}  projected[s,b] × block_duration[s,b]
         =  Σ_t  value[t] × interval_hours

   Both properties hold whenever the aggregation function is the arithmetic
   mean **and** ``block_duration[s,b] = n_occurrences[s,b] × interval_hours``.

**Stage-to-time mapping** (one of):

- ``month`` (int 1-12)        → single calendar month
- ``months`` (list[int])      → multiple calendar months
- ``start_date``/``end_date`` → arbitrary date range (``"YYYY-MM-DD"``)

**Block-to-time mapping** (one of):

- ``hour`` (int 0-23)             → single hour of the day
- ``hours`` (list[int])           → multiple hours of the day
- ``start_hour``/``end_hour``     → hour range (inclusive)
- *(absent)*                      → block *j* (0-indexed) maps to hour *j* mod 24

Output is a DataFrame with ``int32`` index columns ``scenario``, ``stage``,
``block``, a ``float64`` column ``_duration`` (effective block duration in
hours – planning metadata, **not** written to schedule files), and one
``float64`` data column per element.
"""

# pylint: disable=too-many-lines
from __future__ import annotations

import calendar
import copy
import json
import logging
import math
from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd

logger = logging.getLogger(__name__)

_INDEX_COLS = {"scenario", "stage", "block"}
_META_COLS = {"_month", "_hour", "_year", "_dt", "_duration"}


# ---------------------------------------------------------------------------
# Horizon helpers
# ---------------------------------------------------------------------------


def _get_stages(h: dict[str, Any]) -> list[dict[str, Any]]:
    """Return the stage list (supports both ``stages`` and ``stage_array``)."""
    return h.get("stages", h.get("stage_array", []))


def _get_blocks(h: dict[str, Any]) -> list[dict[str, Any]]:
    """Return the flat block list (supports ``blocks`` and ``block_array``)."""
    return h.get("blocks", h.get("block_array", []))


def _get_scenarios(h: dict[str, Any]) -> list[dict[str, Any]]:
    """Return the scenario list from a horizon dict."""
    return h.get("scenarios", h.get("scenario_array", [{"uid": 1}]))


def _get_stage_blocks(
    stage: dict[str, Any], all_blocks: list[dict[str, Any]]
) -> list[dict[str, Any]]:
    """Return the blocks that belong to *stage*.

    If the stage has ``first_block`` and ``count_block`` keys (as produced by
    :func:`make_horizon`), only that slice of *all_blocks* is returned.
    Otherwise every block is considered shared across all stages.
    """
    if "first_block" in stage and "count_block" in stage:
        first = int(stage["first_block"])
        count = int(stage["count_block"])
        return all_blocks[first : first + count]
    return all_blocks


def _sequential_month_range(index: int, total: int) -> list[int]:
    """Sequential month range for the *index*-th item of *total* equal divisions.

    Returns the list of calendar months (1-12) assigned to slot *index* when
    twelve months are divided as evenly as possible into *total* parts.
    """
    n = max(total, 1)
    start_m = math.floor(index * 12 / n) + 1
    end_m = max(math.floor((index + 1) * 12 / n), start_m)
    return list(range(start_m, end_m + 1))


def _sequential_hour_range(index: int, total: int) -> list[int]:
    """Sequential hour range for the *index*-th item of *total* equal divisions.

    Returns the list of hours-of-day (0-23) assigned to slot *index* when
    twenty-four hours are divided as evenly as possible into *total* parts.
    """
    m = max(total, 1)
    start_h = math.floor(index * 24 / m)
    end_h = max(math.floor((index + 1) * 24 / m) - 1, start_h)
    return list(range(start_h, end_h + 1))


def _build_meta_set(df: pd.DataFrame, time_column: str) -> set[str]:
    """Return the set of non-element column names for *df*.

    Includes ``_META_COLS``, ``_INDEX_COLS``, *time_column*, and any column
    that has a datetime64 dtype.
    """
    meta: set[str] = _META_COLS | _INDEX_COLS | {time_column}
    for c in df.columns:
        if pd.api.types.is_datetime64_any_dtype(df[c]):
            meta.add(c)
    return meta


def _stage_months(stage: dict[str, Any], stage_index: int, n_stages: int) -> list[int]:
    """Return the calendar months (1-12) covered by a stage.

    Resolution order:

    1. Explicit ``months`` list.
    2. Scalar ``month``.
    3. Date range ``start_date`` / ``end_date``.
    4. Fallback: sequential distribution – stage *i* of *n_stages* covers
       months ``[floor(i*12/n)+1 … floor((i+1)*12/n)]``.
    """
    if "months" in stage:
        return [int(m) for m in stage["months"]]
    if "month" in stage:
        return [int(stage["month"])]
    if "start_date" in stage and "end_date" in stage:
        start = pd.Timestamp(stage["start_date"])
        end = pd.Timestamp(stage["end_date"])
        months: list[int] = []
        cur = pd.Timestamp(year=start.year, month=start.month, day=1)
        while cur <= end:
            if cur.month not in months:
                months.append(cur.month)
            cur = (
                pd.Timestamp(year=cur.year + 1, month=1, day=1)
                if cur.month == 12
                else pd.Timestamp(year=cur.year, month=cur.month + 1, day=1)
            )
        return months
    # Fallback: even distribution
    return _sequential_month_range(stage_index, n_stages)


def _block_hours(block: dict[str, Any], block_index: int, n_blocks: int) -> list[int]:
    """Return the hours-of-day (0-23) covered by a block.

    Resolution order:

    1. Explicit ``hours`` list.
    2. Scalar ``hour``.
    3. Range ``start_hour`` / ``end_hour`` (inclusive).
    4. Fallback: sequential distribution – block *j* of *n_blocks* covers
       hours ``[floor(j*24/m) … floor((j+1)*24/m)-1]``.
    """
    if "hours" in block:
        return [int(h) % 24 for h in block["hours"]]
    if "hour" in block:
        return [int(block["hour"]) % 24]
    if "start_hour" in block and "end_hour" in block:
        sh = int(block["start_hour"]) % 24
        eh = int(block["end_hour"]) % 24
        return list(range(sh, eh + 1))
    return _sequential_hour_range(block_index, n_blocks)


# ---------------------------------------------------------------------------
# Interval detection
# ---------------------------------------------------------------------------


def _infer_interval_hours(df: pd.DataFrame, time_col: str) -> float:
    """Infer the sampling interval in hours from consecutive timestamps.

    Falls back to ``1.0`` (hourly) when the column is absent or the data has
    fewer than two rows.
    """
    if time_col not in df.columns:
        return 1.0
    ts = pd.to_datetime(df[time_col]).sort_values()
    if len(ts) < 2:
        return 1.0
    diffs = ts.diff().dropna()
    if diffs.empty:
        return 1.0
    median_s = diffs.median().total_seconds()
    return max(median_s / 3600.0, 1e-9)


# ---------------------------------------------------------------------------
# Horizon construction and loading
# ---------------------------------------------------------------------------


def make_horizon(
    year: int,
    n_stages: int = 12,
    n_blocks: int = 24,
    probability_factor: float = 1.0,
    interval_hours: float = 1.0,
) -> dict[str, Any]:
    """Generate a conservation-correct annual planning horizon.

    Block durations are computed as::

        duration = n_days_in_stage × n_hours_per_block × interval_hours

    so that the *period* and *energy* conservation properties hold (see
    module docstring).

    The default structure (``n_stages=12``, ``n_blocks=24``) creates one stage
    per calendar month with one block per hour-of-day.  The 24 blocks for
    January will each have ``duration = 31 h`` (31 occurrences of each hour in
    January), whereas February's blocks will have ``duration = 28 h``, etc.

    Because different months have different numbers of days, the function
    generates **per-stage blocks** (a flat block_array of
    ``n_stages × n_blocks`` entries). Each stage entry records ``first_block``
    and ``count_block`` to select its slice.

    Parameters
    ----------
    year:
        Calendar year; used to compute the exact number of days per month
        (including leap-year handling).
    n_stages:
        Number of planning stages.  Common values:

        - ``12`` → one stage per calendar month  *(default)*
        - ``4``  → one stage per season (Q1–Q4)
        - ``1``  → single annual stage
    n_blocks:
        Number of blocks per stage.  Common values:

        - ``24`` → one block per hour of the day  *(default)*
        - ``1``  → single block per stage (daily average)
    probability_factor:
        Probability weight for the single generated scenario.
    interval_hours:
        Duration of each observation in the input time-series, in hours.
        Use ``1.0`` for hourly data *(default)*, ``0.25`` for 15-minute data,
        ``24.0`` for daily data, etc.

    Returns
    -------
    dict
        Horizon dict suitable for :func:`project_timeseries`.  Contains
        ``stages``, ``blocks``, ``scenarios``, ``year``, and
        ``interval_hours``.
    """
    # --- Stage month ranges ---------------------------------------------------
    stage_months_list: list[list[int]] = []
    for i in range(n_stages):
        stage_months_list.append(_sequential_month_range(i, n_stages))

    # --- Per-stage blocks with conservation-correct durations -----------------
    blocks_list: list[dict[str, Any]] = []
    stages_list: list[dict[str, Any]] = []
    block_uid = 1

    for st_idx, months in enumerate(stage_months_list):
        n_days = sum(calendar.monthrange(year, m)[1] for m in months)
        stage_first_block = block_uid - 1  # 0-indexed position in blocks_list

        for bl_idx in range(n_blocks):
            hours = _sequential_hour_range(bl_idx, n_blocks)
            # duration = total time this block represents in the planning period
            duration = float(n_days * len(hours) * interval_hours)
            blocks_list.append(
                {
                    "uid": block_uid,
                    "hours": hours,
                    "duration": duration,
                }
            )
            block_uid += 1

        stages_list.append(
            {
                "uid": st_idx + 1,
                "months": months,
                "first_block": stage_first_block,
                "count_block": n_blocks,
            }
        )

    return {
        "year": year,
        "scenarios": [{"uid": 1, "probability_factor": probability_factor}],
        "stages": stages_list,
        "blocks": blocks_list,
        "interval_hours": interval_hours,
    }


def load_horizon(path: Path) -> dict[str, Any]:
    """Load a planning horizon definition from a JSON file.

    The JSON must contain at least one of ``stages`` / ``stage_array`` **and**
    ``blocks`` / ``block_array``.

    Parameters
    ----------
    path:
        Path to the JSON horizon file.

    Returns
    -------
    dict
        Horizon dict suitable for :func:`project_timeseries`.

    Raises
    ------
    ValueError
        If the file does not contain stage or block definitions.
    """
    with open(path, encoding="utf-8") as f:
        h: dict[str, Any] = json.load(f)
    if not _get_stages(h):
        raise ValueError(f"Horizon file '{path}' has no stages/stage_array")
    if not _get_blocks(h):
        raise ValueError(f"Horizon file '{path}' has no blocks/block_array")
    return h


# ---------------------------------------------------------------------------
# Time-series loading
# ---------------------------------------------------------------------------

_TIME_COL_ALIASES = (
    "datetime",
    "timestamp",
    "time",
    "date",
    "Datetime",
    "Timestamp",
    "Time",
    "Date",
)


def _detect_time_column(df: pd.DataFrame, requested: str) -> str:
    """Return the time column name, trying common aliases if *requested* absent."""
    if requested in df.columns:
        return requested
    for alt in _TIME_COL_ALIASES:
        if alt in df.columns:
            logger.info(
                "Time column '%s' not found; using '%s' instead", requested, alt
            )
            return alt
    raise ValueError(
        f"Time column '{requested}' not found. Available columns: {list(df.columns)}"
    )


def load_timeseries(
    path: Path,
    time_column: str = "datetime",
) -> tuple[pd.DataFrame, str]:
    """Load a time-series file (CSV or Parquet) and attach helper columns.

    The returned DataFrame contains the original data columns plus three
    helper columns: ``_month`` (1-12), ``_hour`` (0-23), and ``_year``.

    Parameters
    ----------
    path:
        CSV or Parquet file path.
    time_column:
        Name of the datetime column.  Common alternatives are tried
        automatically if the exact name is not found.

    Returns
    -------
    tuple[pd.DataFrame, str]
        ``(df_with_helpers, detected_time_column_name)``
    """
    path = Path(path)
    if path.suffix.lower() == ".parquet":
        df = pd.read_parquet(path)
    else:
        df = pd.read_csv(path)

    col = _detect_time_column(df, time_column)
    df = df.copy()
    df[col] = pd.to_datetime(df[col])
    df["_month"] = df[col].dt.month
    df["_hour"] = df[col].dt.hour
    df["_year"] = df[col].dt.year
    return df, col


# ---------------------------------------------------------------------------
# Core projection
# ---------------------------------------------------------------------------


def _resolve_interval_hours(
    horizon: dict[str, Any], df: pd.DataFrame, time_column: str
) -> float:
    """Return interval_hours from horizon or auto-detected from df."""
    ih = float(horizon.get("interval_hours", 0))
    if ih > 0:
        return ih
    col = (
        time_column
        if time_column in df.columns
        else next((a for a in _TIME_COL_ALIASES if a in df.columns), time_column)
    )
    return _infer_interval_hours(df, col)


def _attach_helpers(df: pd.DataFrame, time_column: str) -> pd.DataFrame:
    """Add _month, _hour, _year columns if not already present."""
    if "_month" in df.columns and "_hour" in df.columns:
        return df
    col = _detect_time_column(df, time_column)
    df = df.copy()
    df[col] = pd.to_datetime(df[col])
    df["_month"] = df[col].dt.month
    df["_hour"] = df[col].dt.hour
    df["_year"] = df[col].dt.year
    return df


def _prepare_df(
    df: pd.DataFrame,
    horizon: dict[str, Any],
    time_column: str,
    year: int | None,
    interval_hours: float | None,
) -> tuple[pd.DataFrame, list[str], float]:
    """Prepare the DataFrame for projection: add helpers, filter year, find element cols."""
    if interval_hours is None:
        interval_hours = _resolve_interval_hours(horizon, df, time_column)

    df = _attach_helpers(df, time_column)

    if year is not None:
        df = df[df["_year"] == year]
        if df.empty:
            raise ValueError(f"No rows found for year {year} in the time-series")

    meta = _build_meta_set(df, time_column)
    element_cols = [c for c in df.columns if c not in meta]
    if not element_cols:
        raise ValueError("No element columns found in time-series DataFrame")

    return df, element_cols, interval_hours


def _agg_cell(
    cell_df: pd.DataFrame,
    element_cols: list[str],
    agg_func: str,
    st_uid: int,
    st_months: list[int],
    bl_uid: int,
    bl_hours: list[int],
) -> dict[str, Any]:
    """Aggregate one (stage, block) cell; return NaN dict if empty."""
    if cell_df.empty:
        logger.debug(
            "No data for stage %d months=%s, block %d hours=%s – NaN",
            st_uid,
            st_months,
            bl_uid,
            bl_hours,
        )
        return {c: np.nan for c in element_cols}
    return getattr(cell_df[element_cols], agg_func)().to_dict()


def _project_scenario_rows(
    scenario_uid: int,
    df: pd.DataFrame,
    stages: list[dict[str, Any]],
    all_blocks: list[dict[str, Any]],
    element_cols: list[str],
    agg_func: str,
    interval_hours: float,
) -> list[dict[str, Any]]:
    """Build all (stage × block) rows for one scenario."""
    rows: list[dict[str, Any]] = []
    n_stages = len(stages)

    for st_idx, stage in enumerate(stages):
        st_uid = int(stage.get("uid", st_idx + 1))
        st_months = _stage_months(stage, st_idx, n_stages)
        stage_df = df[df["_month"].isin(st_months)]
        stage_blocks = _get_stage_blocks(stage, all_blocks)
        n_blk = len(stage_blocks)

        for bl_idx, block in enumerate(stage_blocks):
            bl_uid = int(block.get("uid", bl_idx + 1))
            bl_hours = _block_hours(block, bl_idx, n_blk)
            cell_df = stage_df[stage_df["_hour"].isin(bl_hours)]

            effective_duration = float(len(cell_df)) * interval_hours
            values = _agg_cell(
                cell_df, element_cols, agg_func, st_uid, st_months, bl_uid, bl_hours
            )

            row: dict[str, Any] = {
                "scenario": scenario_uid,
                "stage": st_uid,
                "block": bl_uid,
                "_duration": effective_duration,
            }
            row.update(values)
            rows.append(row)

    return rows


def project_timeseries(
    df: pd.DataFrame,
    horizon: dict[str, Any],
    time_column: str = "datetime",
    agg_func: str = "mean",
    year: int | None = None,
    interval_hours: float | None = None,
) -> pd.DataFrame:
    """Aggregate a time-series DataFrame onto a gtopt planning horizon.

    For every *(scenario, stage, block)* tuple the function selects all rows
    whose timestamp falls within the stage's calendar period **and** within the
    block's hour-of-day window, then applies *agg_func* over all element
    columns.

    The result includes a ``_duration`` column recording the *effective block
    duration* (``n_occurrences × interval_hours``).  This column is planning
    metadata used for conservation verification and for updating the block_array
    in the planning JSON; it is **not** written to the final schedule files by
    :func:`write_schedule`.

    **Conservation properties** (hold for ``agg_func="mean"``)

    - *Period*: ``Σ _duration == total_period``
    - *Energy*: ``Σ (value × _duration) == Σ (original_value × interval_hours)``

    Parameters
    ----------
    df:
        Time-series DataFrame.  Must contain a datetime-typed column (matched
        via *time_column* / common aliases) and numeric element columns.  If
        ``_month``, ``_hour``, ``_year`` helpers are already present (from
        :func:`load_timeseries`) they are reused.
    horizon:
        Planning horizon dict from :func:`load_horizon` or
        :func:`make_horizon`.  Must contain ``stages``/``stage_array`` and
        ``blocks``/``block_array``.  When stages include ``first_block`` and
        ``count_block`` keys (as produced by :func:`make_horizon`), per-stage
        blocks are used.
    time_column:
        Name of the datetime column (used only when helper columns are absent).
    agg_func:
        Aggregation applied per *(stage × block × element)* group.
        One of ``"mean"`` *(default)*, ``"median"``, ``"min"``,
        ``"max"``, ``"sum"``.
    year:
        When given, restrict the time-series to this calendar year before
        aggregating.  Useful when the input spans multiple years.
    interval_hours:
        Duration of each observation in the input time-series, in hours.
        When ``None`` the value is taken from ``horizon["interval_hours"]``
        (if present), otherwise auto-detected from the timestamps, falling
        back to ``1.0`` for hourly data.

    Returns
    -------
    pd.DataFrame
        Schedule DataFrame with ``int32`` columns ``scenario``, ``stage``,
        ``block``, a ``float64`` column ``_duration``, and ``float64`` element
        columns.

    Raises
    ------
    ValueError
        If *agg_func* is unknown, no element columns are found, or *year*
        is given but no rows match.
    """
    _valid_agg = {"mean", "median", "min", "max", "sum"}
    if agg_func not in _valid_agg:
        raise ValueError(
            f"agg_func must be one of {sorted(_valid_agg)}, got '{agg_func}'"
        )

    df, element_cols, interval_hours = _prepare_df(
        df, horizon, time_column, year, interval_hours
    )

    scenarios = _get_scenarios(horizon)
    stages = _get_stages(horizon)
    all_blocks = _get_blocks(horizon)

    rows: list[dict[str, Any]] = []
    for scenario in scenarios:
        scen_uid = int(scenario.get("uid", 1))
        rows.extend(
            _project_scenario_rows(
                scen_uid, df, stages, all_blocks, element_cols, agg_func, interval_hours
            )
        )

    col_order = ["scenario", "stage", "block", "_duration"] + element_cols
    result = pd.DataFrame(rows, columns=col_order)

    for col in result.columns:
        if col in _INDEX_COLS:
            result[col] = result[col].astype("int32")
        else:
            result[col] = result[col].astype("float64")

    return result


# ---------------------------------------------------------------------------
# Conservation utilities
# ---------------------------------------------------------------------------


def energy_conservation_check(
    original_df: pd.DataFrame,
    schedule_df: pd.DataFrame,
    interval_hours: float = 1.0,
    time_column: str = "datetime",
) -> dict[str, float]:
    """Compute energy conservation ratios between original and projected data.

    For a perfectly energy-conserving projection, all ratios equal ``1.0``::

        ratio[col] = Σ(projected[col] × _duration) / Σ(original[col] × interval_hours)

    Parameters
    ----------
    original_df:
        The raw time-series DataFrame (before projection).
    schedule_df:
        The projected schedule DataFrame returned by :func:`project_timeseries`
        (must include the ``_duration`` column).
    interval_hours:
        Duration of each observation in *original_df*, in hours.
    time_column:
        Name of the datetime column in *original_df* (used only to exclude it
        from element column identification).

    Returns
    -------
    dict[str, float]
        Mapping element column → conservation ratio. ``1.0`` = perfect.
        A value slightly different from 1.0 may occur when some (stage, block)
        windows have no data (NaN entries).

    Raises
    ------
    ValueError
        If *schedule_df* is missing the ``_duration`` column.
    """
    if "_duration" not in schedule_df.columns:
        raise ValueError(
            "schedule_df must contain the '_duration' column. "
            "Ensure project_timeseries() was called without dropping it."
        )

    # Element columns present in both DataFrames
    meta = _build_meta_set(original_df, time_column)
    element_cols = [
        c for c in schedule_df.columns if c not in meta and c in original_df.columns
    ]

    ratios: dict[str, float] = {}
    for col in element_cols:
        orig_energy = float((original_df[col] * interval_hours).sum())
        proj_energy = float((schedule_df[col] * schedule_df["_duration"]).sum())
        if abs(orig_energy) > 1e-10:
            ratios[col] = proj_energy / orig_energy
        else:
            ratios[col] = 1.0 if abs(proj_energy) < 1e-10 else float("inf")

    return ratios


def update_horizon_durations(
    horizon: dict[str, Any],
    schedule_df: pd.DataFrame,
) -> dict[str, Any]:
    """Return a copy of *horizon* with block durations updated from the schedule.

    Uses the ``_duration`` column of *schedule_df* (from
    :func:`project_timeseries`) to set each block's ``duration`` to the
    effective value derived from the actual input data, ensuring
    period-and-energy conservation.

    Parameters
    ----------
    horizon:
        Original planning horizon dict.
    schedule_df:
        Projected schedule DataFrame (must contain ``_duration`` column).

    Returns
    -------
    dict
        Deep copy of *horizon* with updated block ``duration`` values.
    """
    if "_duration" not in schedule_df.columns:
        return horizon

    h = copy.deepcopy(horizon)
    blocks = _get_blocks(h)

    # Use first scenario to build block_uid → effective_duration map
    first_scen = int(schedule_df["scenario"].iloc[0]) if not schedule_df.empty else 1
    sub = schedule_df[schedule_df["scenario"] == first_scen]
    dur_map: dict[int, float] = dict(zip(sub["block"].astype(int), sub["_duration"]))

    for block in blocks:
        uid = int(block["uid"])
        if uid in dur_map:
            block["duration"] = dur_map[uid]

    return h


# ---------------------------------------------------------------------------
# Schedule output
# ---------------------------------------------------------------------------


def write_schedule(
    df: pd.DataFrame,
    output_path: Path,
    output_format: str = "parquet",
    compression: str = "gzip",
) -> None:
    """Write a projected schedule DataFrame to Parquet or CSV.

    Internal metadata columns (those whose names begin with ``_``, such as
    ``_duration``) are **dropped** before writing so that the output matches
    the standard gtopt schedule format:
    ``scenario  stage  block  uid:1  uid:2  …``

    The parent directory is created automatically if it does not exist.

    Parameters
    ----------
    df:
        Schedule DataFrame (from :func:`project_timeseries`).
    output_path:
        Destination file path.
    output_format:
        ``"parquet"`` *(default)* or ``"csv"``.
    compression:
        Parquet compression codec (default ``"gzip"``).
        Pass ``""`` to disable.

    Raises
    ------
    ValueError
        If *output_format* is not ``"parquet"`` or ``"csv"``.
    """
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    # Drop metadata columns (starting with '_')
    write_df = df.drop(
        columns=[c for c in df.columns if c.startswith("_")], errors="ignore"
    )

    if output_format == "parquet":
        comp = compression if compression else None
        write_df.to_parquet(output_path, index=False, compression=comp)
    elif output_format == "csv":
        write_df.to_csv(output_path, index=False)
    else:
        raise ValueError(
            f"output_format must be 'parquet' or 'csv', got '{output_format}'"
        )

    logger.info(
        "Written: %s  (%d rows × %d data cols)",
        output_path,
        len(write_df),
        len(write_df.columns),
    )


# ---------------------------------------------------------------------------
# High-level batch conversion
# ---------------------------------------------------------------------------


def convert_timeseries(
    input_paths: list[Path],
    output_dir: Path,
    horizon: dict[str, Any],
    time_column: str = "datetime",
    agg_func: str = "mean",
    year: int | None = None,
    interval_hours: float | None = None,
    output_format: str = "parquet",
    compression: str = "gzip",
    output_horizon_path: Path | None = None,
) -> dict[str, Path]:
    """Convert a batch of time-series files to gtopt schedule files.

    Each input file is projected onto *horizon* independently and written to
    *output_dir* with the same stem and the appropriate extension.

    When *output_horizon_path* is given the function writes an updated horizon
    JSON whose block ``duration`` values reflect the effective durations
    computed from the **first** input file, satisfying the period- and
    energy-conservation properties.

    Parameters
    ----------
    input_paths:
        Input time-series files (CSV or Parquet).
    output_dir:
        Directory for the output schedule files.
    horizon:
        Planning horizon dict from :func:`load_horizon` or
        :func:`make_horizon`.
    time_column:
        Name of the datetime column in the input files.
    agg_func:
        Aggregation function: ``"mean"`` *(default)*, ``"median"``,
        ``"min"``, ``"max"``, ``"sum"``.
    year:
        Optional calendar-year filter applied to every input file.
    interval_hours:
        Duration of each observation in hours.  Auto-detected when ``None``.
    output_format:
        ``"parquet"`` *(default)* or ``"csv"``.
    compression:
        Parquet compression codec; pass ``""`` to disable.
    output_horizon_path:
        When given, write the duration-updated horizon JSON to this path.

    Returns
    -------
    dict[str, Path]
        Mapping from input file stem to output :class:`~pathlib.Path`.
    """
    output_dir = Path(output_dir)
    ext = "parquet" if output_format == "parquet" else "csv"
    output_paths: dict[str, Path] = {}
    updated_horizon: dict[str, Any] | None = None

    for idx, raw in enumerate(input_paths):
        input_path = Path(raw)
        logger.info("Processing %s", input_path)
        ts_df, _ = load_timeseries(input_path, time_column)

        result_df = project_timeseries(
            ts_df,
            horizon=horizon,
            time_column=time_column,
            agg_func=agg_func,
            year=year,
            interval_hours=interval_hours,
        )

        # Use the first input file to build the updated horizon
        if idx == 0 and output_horizon_path is not None:
            updated_horizon = update_horizon_durations(horizon, result_df)

        out_path = output_dir / f"{input_path.stem}.{ext}"
        write_schedule(result_df, out_path, output_format, compression)
        output_paths[input_path.stem] = out_path

    if output_horizon_path is not None and updated_horizon is not None:
        output_horizon_path = Path(output_horizon_path)
        output_horizon_path.parent.mkdir(parents=True, exist_ok=True)
        with open(output_horizon_path, "w", encoding="utf-8") as f:
            json.dump(updated_horizon, f, indent=2)
        logger.info("Written updated horizon: %s", output_horizon_path)

    return output_paths


# ---------------------------------------------------------------------------
# Hour-block map construction
# ---------------------------------------------------------------------------


def build_hour_block_map(
    horizon: dict[str, Any],
    year: int | None = None,
) -> list[dict[str, Any]]:
    """Build a sequential hour-to-(stage, block) mapping from a planning horizon.

    The map lists one entry per processed observation in the order they would
    appear when stepping through the calendar: for each stage (in order), for
    each day that stage covers, for each hour-of-day that each block covers.
    Entry ``i`` has a 0-based ``"hour"`` index that equals ``i``.

    This map is stored in the planning JSON under the key
    ``"hour_block_map"`` so that hourly block-level solver outputs can be
    expanded back into a full hourly time-series.

    Parameters
    ----------
    horizon:
        Planning horizon dict from :func:`load_horizon` or
        :func:`make_horizon`.  Must contain ``stages``/``stage_array`` and
        ``blocks``/``block_array``.  The ``year`` key is used when *year* is
        ``None`` to compute calendar-accurate day counts.
    year:
        Calendar year used to compute the number of days per stage.  When
        ``None``, falls back to ``horizon["year"]`` if present, otherwise
        assumes 365 days per stage (for approximate maps).

    Returns
    -------
    list[dict[str, Any]]
        Ordered list of entries ``{"hour": i, "stage": stage_uid,
        "block": block_uid}`` with consecutive 0-based ``hour`` values.

    Examples
    --------
    >>> h = make_horizon(2023, n_stages=2, n_blocks=2)
    >>> m = build_hour_block_map(h, year=2023)
    >>> m[0]
    {'hour': 0, 'stage': 1, 'block': 1}
    """
    resolved_year: int | None = year if year is not None else horizon.get("year")

    stages = _get_stages(horizon)
    all_blocks = _get_blocks(horizon)
    n_stages = len(stages)

    entries: list[dict[str, Any]] = []
    hour_idx = 0

    for st_idx, stage in enumerate(stages):
        st_uid = int(stage.get("uid", st_idx + 1))
        st_months = _stage_months(stage, st_idx, n_stages)

        if resolved_year is not None:
            n_days = sum(calendar.monthrange(resolved_year, m)[1] for m in st_months)
        else:
            # Approximate: distribute 365 days evenly over stages
            n_days = max(round(365 / n_stages), 1)

        stage_blocks = _get_stage_blocks(stage, all_blocks)
        n_blk = len(stage_blocks)

        for _ in range(n_days):
            for bl_idx, block in enumerate(stage_blocks):
                bl_uid = int(block.get("uid", bl_idx + 1))
                bl_hours = _block_hours(block, bl_idx, n_blk)
                for _h in bl_hours:
                    entries.append({"hour": hour_idx, "stage": st_uid, "block": bl_uid})
                    hour_idx += 1

    return entries


# ---------------------------------------------------------------------------
# Output-hour reconstruction
# ---------------------------------------------------------------------------


def _read_output_table(path: Path) -> pd.DataFrame | None:
    """Read a CSV or Parquet output file.

    Returns ``None`` if the file is unreadable or does not contain the
    required ``scenario``, ``stage``, and ``block`` columns.
    """
    try:
        if path.suffix.lower() == ".parquet":
            df = pd.read_parquet(path)
        else:
            df = pd.read_csv(path)
    except Exception as exc:  # pylint: disable=broad-except
        logger.debug("Failed to read %s: %s", path, exc)
        return None

    # Must have at least scenario, stage, block columns
    if not {"scenario", "stage", "block"}.issubset(df.columns):
        return None
    return df


def _expand_block_rows(
    df: pd.DataFrame,
    uid_cols: list[str],
    hour_map_df: pd.DataFrame,
) -> pd.DataFrame:
    """Expand a (scenario, stage, block) DataFrame into per-hour rows.

    Merges *df* with *hour_map_df* on ``(stage, block)`` so that each
    original row produces one output row per mapped hour.  Returns a
    DataFrame with columns ``scenario``, ``hour``, and all *uid_cols*.
    """
    keep = ["scenario", "stage", "block"] + uid_cols
    merged = df[keep].merge(hour_map_df, on=["stage", "block"], how="inner")
    return merged.drop(columns=["stage", "block"])


def _write_hourly_result(
    result: pd.DataFrame,
    out_path: Path,
    output_format: str,
) -> None:
    """Write a reconstructed hourly DataFrame to CSV or Parquet."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if output_format == "parquet":
        result.to_parquet(out_path, index=False)
    else:
        result.to_csv(out_path, index=False)


def reconstruct_output_hours(
    output_dir: Path,
    hour_block_map: list[dict[str, Any]],
    output_hour_dir: Path | None = None,
    output_format: str = "csv",
) -> dict[str, Path]:
    """Expand block-level gtopt output files into hourly time-series.

    Reads every CSV/Parquet file under *output_dir* that contains
    ``scenario``, ``stage``, and ``block`` columns, then replaces the
    ``(stage, block)`` dimensions with a sequential ``hour`` index (0-based)
    using *hour_block_map*.  The resulting files are written to
    *output_hour_dir* preserving the subdirectory structure of *output_dir*.

    The output format is ``scenario, hour, uid:1, uid:2, …`` — one row per
    *(scenario, hour)* pair.

    Parameters
    ----------
    output_dir:
        Directory containing gtopt solution files (e.g. ``output/Generator/``
        subdirectories with ``generation_sol.csv``).
    hour_block_map:
        Ordered list of ``{"hour": i, "stage": s, "block": b}`` dicts as
        returned by :func:`build_hour_block_map`.  Length equals the total
        number of processed hours; each entry's ``"hour"`` field is the
        0-based sequential hour index assigned to that observation.
    output_hour_dir:
        Destination directory.  Defaults to a sibling of *output_dir* named
        ``output_hour``.
    output_format:
        ``"csv"`` *(default)* or ``"parquet"``.

    Returns
    -------
    dict[str, Path]
        Mapping from relative file path string to written :class:`~pathlib.Path`.

    Raises
    ------
    ValueError
        If *output_format* is not ``"csv"`` or ``"parquet"``.
    """
    if output_format not in ("csv", "parquet"):
        raise ValueError(
            f"output_format must be 'csv' or 'parquet', got '{output_format}'"
        )

    output_dir = Path(output_dir)
    if output_hour_dir is None:
        output_hour_dir = output_dir.parent / "output_hour"
    output_hour_dir = Path(output_hour_dir)

    # Build a lookup DataFrame for vectorized merge: columns stage, block, hour
    hour_map_df = pd.DataFrame(hour_block_map, columns=["hour", "stage", "block"])
    hour_map_df["stage"] = hour_map_df["stage"].astype("int32")
    hour_map_df["block"] = hour_map_df["block"].astype("int32")

    written: dict[str, Path] = {}
    glob_patterns = ["**/*.csv", "**/*.parquet"]
    seen: set[Path] = set()

    for pattern in glob_patterns:
        for src_path in sorted(output_dir.glob(pattern)):
            if src_path in seen:
                continue
            seen.add(src_path)
            df = _read_output_table(src_path)
            if df is None:
                continue

            uid_cols = [
                c for c in df.columns if c not in {"scenario", "stage", "block"}
            ]
            if not uid_cols:
                continue

            result = _expand_block_rows(df, uid_cols, hour_map_df)

            if result.empty:
                logger.debug("No matching hour entries for %s - skipping", src_path)
                continue

            result["scenario"] = result["scenario"].astype("int32")
            result["hour"] = result["hour"].astype("int32")
            result = result.sort_values(["scenario", "hour"]).reset_index(drop=True)

            # Mirror subdirectory structure
            rel = src_path.relative_to(output_dir)
            suffix = ".parquet" if output_format == "parquet" else ".csv"
            out_path = output_hour_dir / rel.with_suffix(suffix)
            _write_hourly_result(result, out_path, output_format)

            written[str(rel)] = out_path
            logger.info("Reconstructed %d hourly rows -> %s", len(result), out_path)

    return written


def write_output_hours(
    case_json_path: Path,
    output_dir: Path | None = None,
    output_hour_dir: Path | None = None,
    output_format: str = "csv",
) -> dict[str, Path]:
    """Reconstruct hourly time-series from a gtopt case output directory.

    Reads the ``hour_block_map`` embedded in *case_json_path* (added by
    :func:`build_hour_block_map` when the case was created), then calls
    :func:`reconstruct_output_hours` on the ``output/`` directory next to
    the JSON file.

    Parameters
    ----------
    case_json_path:
        Path to the gtopt planning JSON file (e.g. ``bat_4b_2023.json``).
        Must contain a top-level ``"hour_block_map"`` array.
    output_dir:
        Path to the directory containing solver output files.  Defaults to a
        sub-directory named ``output`` next to *case_json_path*.
    output_hour_dir:
        Destination directory for the hourly reconstructions.  Defaults to a
        sibling of *output_dir* named ``output_hour``.
    output_format:
        ``"csv"`` *(default)* or ``"parquet"``.

    Returns
    -------
    dict[str, Path]
        Mapping from relative file path string to written :class:`~pathlib.Path`.

    Raises
    ------
    KeyError
        If the JSON file does not contain a ``"hour_block_map"`` key.
    """
    case_json_path = Path(case_json_path)
    with open(case_json_path, encoding="utf-8") as f:
        case_json: dict[str, Any] = json.load(f)

    if "hour_block_map" not in case_json:
        raise KeyError(
            f"'hour_block_map' not found in '{case_json_path}'.  "
            "Re-generate the case with build_hour_block_map() to add it."
        )

    hour_block_map: list[dict[str, Any]] = case_json["hour_block_map"]

    if output_dir is None:
        output_dir = case_json_path.parent / "output"
    output_dir = Path(output_dir)

    return reconstruct_output_hours(
        output_dir,
        hour_block_map,
        output_hour_dir=output_hour_dir,
        output_format=output_format,
    )
