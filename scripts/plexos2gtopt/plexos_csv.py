"""Readers for CEN PCP per-class CSV time-series.

Two file layouts cover everything in ``DATOS{date}.zip``:

* **Long format**: ``NAME, YEAR, MONTH, DAY, PERIOD, BAND, VALUE`` —
  used by ``Gen_*.csv`` and ``Hydro_*.csv``. ``BAND`` carries
  piecewise segments (heat-rate breakpoints, multi-step bid curves).
* **Wide format**: ``YEAR, MONTH, DAY, PERIOD, <obj1>, <obj2>, …`` —
  used by ``Nod_Load.csv``.  One column per object, one row per
  period.

``PERIOD`` is 1-based hour-of-day (1..24 for the PCP daily horizon).

This module returns ``dict[name -> list[float]]`` rather than pandas
DataFrames so the parsers stay zero-dep at import time and pylint
doesn't have to chase pandas's signature changes across versions.
The conversion functions are intentionally narrow — anything that
needs DataFrame-grade processing should consume the dict and build
its own frame downstream.
"""

from __future__ import annotations

import csv
import logging
from pathlib import Path


logger = logging.getLogger(__name__)


# Canonical PCP horizon: 24 hourly periods (1..24).
DEFAULT_PERIODS = 24


def read_long(
    path: Path | str,
    *,
    band: int = 1,
    periods: int = DEFAULT_PERIODS,
) -> dict[str, list[float]]:
    """Parse a long-format CEN CSV into ``{name -> [v_period_1, …]}``.

    Args:
        path: CSV path (``Gen_Rating.csv``, ``Hydro_WaterFlows.csv``, …).
        band: Which BAND segment to return. Defaults to ``1`` (the
            first / primary segment); multi-band readers should call
            this once per band and merge.
        periods: Expected number of periods per name. Series shorter
            than ``periods`` are right-padded with ``0.0``; longer ones
            are truncated and a warning is logged.

    Returns:
        Dict keyed by ``NAME``; values are lists of length ``periods``.
    """
    out: dict[str, list[float]] = {}
    with Path(path).open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        # CEN CSVs sometimes ship a BOM on the first header column.
        if reader.fieldnames:
            reader.fieldnames = [h.lstrip("﻿") for h in reader.fieldnames]
        for row in reader:
            try:
                row_band = int(row.get("BAND", "1") or "1")
            except ValueError:
                continue
            if row_band != band:
                continue
            name = (row.get("NAME") or "").strip()
            if not name:
                continue
            try:
                period = int(row.get("PERIOD", "0") or "0")
                value = float(row.get("VALUE", "0") or "0")
            except ValueError:
                continue
            if period < 1 or period > periods:
                logger.debug("%s: skipping out-of-range period %d", path, period)
                continue
            series = out.setdefault(name, [0.0] * periods)
            series[period - 1] = value
    return out


def read_wide(
    path: Path | str,
    *,
    periods: int = DEFAULT_PERIODS,
    band: int = 1,
    drop_zero_cols: bool = True,
) -> dict[str, list[float]]:
    """Parse a wide-format CEN CSV into ``{column -> [v_period_1, …]}``.

    Args:
        path: CSV path (``Nod_Load.csv``, ``Lin_Units.csv``, …).
        periods: Expected number of periods (rows). Profiles shorter
            than ``periods`` are right-padded; longer are truncated.
        band: When the CSV has a ``BAND`` meta column (e.g.
            ``Lin_Units.csv``), keep only rows matching this band index.
        drop_zero_cols: When True (default), columns that end up all-
            zero are dropped from the result — matches the demand
            extractor's expectation that only buses with non-zero load
            survive. Set False for boolean-style columns (e.g.
            ``Lin_Units.csv``'s 0/1 availability) where a zero means
            "offline this hour", not "drop me".

    Returns:
        Dict keyed by column name (bus / reservoir / line); values are
        lists of length ``periods``.

    The first four-or-five columns
    (``YEAR``, ``MONTH``, ``DAY``, ``PERIOD``, optional ``BAND``) are
    interpreted as metadata; everything else is a data column.
    """
    out: dict[str, list[float]] = {}
    meta_cols = {"YEAR", "MONTH", "DAY", "PERIOD", "BAND"}
    with Path(path).open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            return out
        # Strip BOM and identify data columns once.
        reader.fieldnames = [h.lstrip("﻿") for h in reader.fieldnames]
        has_band = "BAND" in reader.fieldnames
        data_cols = [h for h in reader.fieldnames if h not in meta_cols]
        for col in data_cols:
            out[col] = [0.0] * periods
        for row in reader:
            if has_band:
                try:
                    row_band = int(row.get("BAND", "1") or "1")
                except ValueError:
                    continue
                if row_band != band:
                    continue
            try:
                period = int(row.get("PERIOD", "0") or "0")
            except ValueError:
                continue
            if period < 1 or period > periods:
                continue
            for col in data_cols:
                raw = row.get(col, "")
                if raw is None or raw == "":
                    continue
                try:
                    out[col][period - 1] = float(raw)
                except ValueError:
                    continue
    if drop_zero_cols:
        return {k: v for k, v in out.items() if any(x != 0.0 for x in v)}
    return out


__all__ = ["DEFAULT_PERIODS", "read_long", "read_wide"]
