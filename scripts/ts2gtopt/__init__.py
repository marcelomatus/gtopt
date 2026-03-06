"""ts2gtopt – Project time-series data onto a gtopt planning horizon.

Given an hourly (or finer) time-series and a planning horizon definition
(scenarios, stages, blocks), this package aggregates the raw values into
representative block values suitable for direct use as gtopt schedule files
(Parquet or CSV) while preserving both the total period and accumulated
energy of the original time-series.

Public API
----------
make_horizon(year, n_stages, n_blocks, interval_hours)
    Auto-generate a conservation-correct annual planning horizon dict.

load_horizon(path)
    Load a planning horizon definition from a JSON file.

project_timeseries(df, horizon, ...)
    Aggregate a time-series DataFrame onto the given horizon.
    Returns a DataFrame with a ``_duration`` column for conservation checks.

write_schedule(df, output_path, ...)
    Write a projected schedule to Parquet or CSV (drops ``_duration``).

energy_conservation_check(original_df, schedule_df, interval_hours)
    Compute energy conservation ratios between original and projected data.

update_horizon_durations(horizon, schedule_df)
    Return a copy of the horizon with block durations updated from actual data.

convert_timeseries(input_paths, output_dir, horizon, ...)
    High-level batch conversion (load → project → write) with optional
    output of the duration-updated horizon JSON.

build_hour_block_map(horizon, year)
    Build a sequential hour-to-(stage, block) mapping from a planning horizon.
    Returns a list of ``{"hour": i, "stage": s, "block": b}`` dicts.

reconstruct_output_hours(output_dir, hour_block_map, ...)
    Expand block-level gtopt output files into hourly time-series.
    Writes ``output_hour/`` with ``scenario, hour, uid:X`` format.

write_output_hours(case_json_path, ...)
    Convenience wrapper: reads ``hour_block_map`` from the case JSON and
    calls :func:`reconstruct_output_hours`.
"""

from .ts2gtopt import (
    build_hour_block_map,
    convert_timeseries,
    energy_conservation_check,
    load_horizon,
    load_timeseries,
    make_horizon,
    project_timeseries,
    reconstruct_output_hours,
    update_horizon_durations,
    write_output_hours,
    write_schedule,
)
from .main import main

__all__ = [
    "build_hour_block_map",
    "convert_timeseries",
    "energy_conservation_check",
    "load_horizon",
    "load_timeseries",
    "make_horizon",
    "project_timeseries",
    "reconstruct_output_hours",
    "update_horizon_durations",
    "write_output_hours",
    "write_schedule",
    "main",
]
