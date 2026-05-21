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
    n_days: int = 1,
    day_offset: int = 0,
) -> dict[str, list[float]]:
    """Parse a long-format CEN CSV into ``{name -> [v_t1, v_t2, …]}``.

    Args:
        path: CSV path (``Gen_Rating.csv``, ``Hydro_WaterFlows.csv``, …).
        band: Which BAND segment to return. Defaults to ``1`` (the
            first / primary segment); multi-band readers should call
            this once per band and merge.
        periods: Periods PER DAY (typically 24 for hourly CEN PCP).
        n_days: How many consecutive days to extract; same semantics as
            in :func:`read_wide`.  ``n_days = 1`` (default) keeps the
            legacy per-day behaviour.
        day_offset: 0-indexed offset of the first day to extract.

    Returns:
        Dict keyed by ``NAME``; values are lists of length
        ``periods * n_days``.  Multi-day rows are concatenated in
        calendar order (first (Y,M,D) tuple seen = day-0).
    """
    out_len = periods * n_days
    out: dict[str, list[float]] = {}
    day_keys_seen: dict[tuple[str, str, str], int] = {}
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
            day_key = (
                row.get("YEAR", "") or "",
                row.get("MONTH", "") or "",
                row.get("DAY", "") or "",
            )
            if day_key not in day_keys_seen:
                day_keys_seen[day_key] = len(day_keys_seen)
            day_idx = day_keys_seen[day_key] - day_offset
            if day_idx < 0 or day_idx >= n_days:
                continue
            slot = day_idx * periods + (period - 1)
            series = out.setdefault(name, [0.0] * out_len)
            series[slot] = value
    return out


def read_wide(
    path: Path | str,
    *,
    periods: int = DEFAULT_PERIODS,
    band: int = 1,
    drop_zero_cols: bool = True,
    n_days: int = 1,
    day_offset: int = 0,
) -> dict[str, list[float]]:
    """Parse a wide-format CEN CSV into ``{column -> [v_t1, v_t2, …]}``.

    Args:
        path: CSV path (``Nod_Load.csv``, ``Lin_Units.csv``, …).
        periods: Periods PER DAY (typically 24 for hourly CEN PCP).
        band: When the CSV has a ``BAND`` meta column (e.g.
            ``Lin_Units.csv``), keep only rows matching this band index.
        drop_zero_cols: When True (default), columns that end up all-
            zero are dropped from the result.
        n_days: How many consecutive days to extract.  ``n_days = 1``
            (default) keeps the legacy day-1-only behavior; ``n_days = 7``
            concatenates a full PCP week into a 168-element profile per
            column.  Rows beyond ``day_offset + n_days`` are ignored.
        day_offset: 0-indexed offset of the first day to extract.  The
            FIRST distinct ``(YEAR, MONTH, DAY)`` tuple in the CSV is
            day-0; ``day_offset = 0`` starts at the bundle's first day.

    Returns:
        Dict keyed by column name (bus / reservoir / line); values are
        lists of length ``periods * n_days``.

    The first four-or-five columns
    (``YEAR``, ``MONTH``, ``DAY``, ``PERIOD``, optional ``BAND``) are
    interpreted as metadata; everything else is a data column.  When
    ``n_days > 1`` the per-day rows are concatenated in calendar order
    (day-0 fills slots 0..periods-1, day-1 fills periods..2·periods-1,
    etc.).
    """
    out_len = periods * n_days
    out: dict[str, list[float]] = {}
    meta_cols = {"YEAR", "MONTH", "DAY", "PERIOD", "BAND"}
    day_keys_seen: dict[tuple[str, str, str], int] = {}
    with Path(path).open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames is None:
            return out
        # Strip BOM and identify data columns once.
        reader.fieldnames = [h.lstrip("﻿") for h in reader.fieldnames]
        has_band = "BAND" in reader.fieldnames
        data_cols = [h for h in reader.fieldnames if h not in meta_cols]
        for col in data_cols:
            out[col] = [0.0] * out_len
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
            # Identify which day this row belongs to.  CSV row order is
            # taken as authoritative — the first (Y,M,D) seen is day-0.
            day_key = (
                row.get("YEAR", "") or "",
                row.get("MONTH", "") or "",
                row.get("DAY", "") or "",
            )
            if day_key not in day_keys_seen:
                day_keys_seen[day_key] = len(day_keys_seen)
            day_idx = day_keys_seen[day_key] - day_offset
            if day_idx < 0 or day_idx >= n_days:
                continue
            slot = day_idx * periods + (period - 1)
            for col in data_cols:
                raw = row.get(col, "")
                if raw is None or raw == "":
                    continue
                try:
                    out[col][slot] = float(raw)
                except ValueError:
                    continue
    if drop_zero_cols:
        return {k: v for k, v in out.items() if any(x != 0.0 for x in v)}
    return out


__all__ = ["DEFAULT_PERIODS", "read_long", "read_wide"]
