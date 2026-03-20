# SPDX-License-Identifier: BSD-3-Clause
"""Resolve FieldSched file references to numeric values.

When a gtopt JSON field (e.g. ``pmax``, ``lmax``, ``tmax_ab``) contains a
string like ``"pmax"`` instead of a number, it is a *file reference* pointing
to a Parquet or CSV file at ``<input_dir>/<ClassName>/<field>.parquet``.

This module reads those files and extracts scalar values for a given
(scenario, stage, block) selection, allowing the diagram to display real
numbers instead of placeholder strings.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)


class FieldSchedResolver:
    """Resolve FieldSched file references to numeric values.

    Parameters
    ----------
    base_dir
        Path to the case directory (where the JSON file lives).
    input_dir
        The ``options.input_directory`` value (often ``"."``).
    scenario
        Scenario UID to select (0-based index if not found by UID).
    stage
        Stage UID to select (0-based index if not found by UID).
    block
        Block UID to select (0-based index if not found by UID).
    """

    def __init__(
        self,
        base_dir: str | Path,
        input_dir: str = ".",
        scenario: int = 1,
        stage: int = 1,
        block: int = 1,
    ) -> None:
        self._base = Path(base_dir)
        self._input_dir = input_dir
        self._scenario = scenario
        self._stage = stage
        self._block = block
        self._cache: dict[tuple[str, str], dict[int, float]] = {}

    def _root(self) -> Path:
        if self._input_dir and self._input_dir != ".":
            return self._base / self._input_dir
        return self._base

    def resolve(
        self,
        class_name: str,
        field_name: str,
        uid: int | str | None,
        fallback: Any = None,
    ) -> float | Any:
        """Resolve a FieldSched value for a specific element.

        Parameters
        ----------
        class_name
            Element class directory (e.g. ``"Generator"``, ``"Demand"``).
        field_name
            Field name / file reference (e.g. ``"pmax"``, ``"lmax"``).
        uid
            Element UID to look up in the ``uid:N`` columns.
        fallback
            Value to return if resolution fails.

        Returns
        -------
        float | Any
            The resolved numeric value, or *fallback* if not found.
        """
        if uid is None:
            return fallback

        cache_key = (class_name, field_name)
        if cache_key not in self._cache:
            self._cache[cache_key] = self._load_file(class_name, field_name)

        uid_map = self._cache[cache_key]
        uid_int = int(uid) if not isinstance(uid, int) else uid
        val = uid_map.get(uid_int)
        return val if val is not None else fallback

    def _load_file(self, class_name: str, field_name: str) -> dict[int, float]:
        """Load a Parquet/CSV file and extract values for the selected row."""
        # Handle cross-class references like "Flow@discharge"
        actual_class = class_name
        actual_field = field_name
        if "@" in field_name:
            parts = field_name.split("@", 1)
            actual_class = parts[0]
            actual_field = parts[1]

        root = self._root()
        parquet_path = root / actual_class / f"{actual_field}.parquet"
        csv_path = root / actual_class / f"{actual_field}.csv"

        try:
            if parquet_path.exists():
                return self._read_parquet(parquet_path)
            if csv_path.exists():
                return self._read_csv(csv_path)
        except Exception:  # pylint: disable=broad-except
            logger.debug("Failed to read %s/%s.parquet/csv", actual_class, actual_field)
        return {}

    def _read_parquet(self, path: Path) -> dict[int, float]:
        """Read Parquet and return {uid: value} for the selected row."""
        try:
            import pyarrow.parquet as pq  # noqa: PLC0415
        except ImportError:
            return {}

        table = pq.read_table(path)
        df = table.to_pandas()
        return self._extract_row(df)

    def _read_csv(self, path: Path) -> dict[int, float]:
        """Read CSV and return {uid: value} for the selected row."""
        try:
            import pandas as pd  # noqa: PLC0415
        except ImportError:
            return {}

        df = pd.read_csv(path)
        return self._extract_row(df)

    def _extract_row(self, df: "Any") -> dict[int, float]:
        """Extract values from a DataFrame for the selected scenario/stage/block."""
        # Filter by scenario, stage, block if columns exist
        if "scenario" in df.columns:
            match = df[df["scenario"] == self._scenario]
            if match.empty:
                # Fall back to first scenario
                first_scen = df["scenario"].iloc[0] if len(df) > 0 else None
                if first_scen is not None:
                    match = df[df["scenario"] == first_scen]
                else:
                    match = df
            df = match

        if "stage" in df.columns:
            match = df[df["stage"] == self._stage]
            if match.empty:
                first_stage = df["stage"].iloc[0] if len(df) > 0 else None
                if first_stage is not None:
                    match = df[df["stage"] == first_stage]
                else:
                    match = df
            df = match

        if "block" in df.columns:
            match = df[df["block"] == self._block]
            if match.empty:
                first_block = df["block"].iloc[0] if len(df) > 0 else None
                if first_block is not None:
                    match = df[df["block"] == first_block]
                else:
                    match = df
            df = match

        # Extract uid:N columns
        result: dict[int, float] = {}
        for col in df.columns:
            if col.startswith("uid:"):
                try:
                    uid = int(col.split(":")[1])
                    val = df[col].iloc[0] if len(df) > 0 else None
                    if val is not None:
                        result[uid] = float(val)
                except (ValueError, IndexError):
                    pass
        return result
