# -*- coding: utf-8 -*-
"""Parser for PLP indhor.csv files mapping calendar hours to block numbers.

The file has columns: Año, Mes, Dia, Hora, Bloque (all integers, 1-based Hora
and Bloque).  It records for every calendar hour in the PLP horizon which
PLP block that hour belongs to.  This mapping is used to reconstruct hourly
time-series from block-level gtopt solver output.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional

import pandas as pd


class IndhorParser:
    """Parser for indhor.csv files containing the hour-to-block mapping.

    The CSV has columns:
        Año  – calendar year  (int)
        Mes  – month 1-12     (int)
        Dia  – day 1-31       (int)
        Hora – hour of day 1-24  (int, 1-based PLP convention)
        Bloque – PLP block number (int, 1-based)

    After parsing, ``self.df`` is a :class:`pandas.DataFrame` with
    column names normalised to lower-case ASCII: ``year``, ``month``,
    ``day``, ``hour``, ``block`` — all ``int32``.  ``hour`` is kept
    1-based (1-24) to match PLP convention.
    """

    _CSV_ENCODINGS = ("utf-8", "latin-1")
    _EXPECTED_COLUMNS = {
        "año": "year",
        "mes": "month",
        "dia": "day",
        "hora": "hour",
        "bloque": "block",
    }

    def __init__(self, file_path: str | Path) -> None:
        self.file_path = Path(file_path)
        self._df: Optional[pd.DataFrame] = None

    @property
    def df(self) -> Optional[pd.DataFrame]:
        """Return the parsed DataFrame, or None if not yet parsed."""
        return self._df

    @property
    def is_empty(self) -> bool:
        return self._df is None or self._df.empty

    def parse(self) -> None:
        """Read and normalise indhor.csv.

        Raises:
            FileNotFoundError: if the file does not exist.
            ValueError: if required columns are missing.
        """
        if not self.file_path.exists():
            raise FileNotFoundError(f"indhor.csv not found: {self.file_path}")

        df: Optional[pd.DataFrame] = None
        for enc in self._CSV_ENCODINGS:
            try:
                df = pd.read_csv(self.file_path, encoding=enc)
                break
            except UnicodeDecodeError:
                continue
        if df is None:
            raise ValueError(
                f"Cannot decode {self.file_path} with any supported encoding"
            )

        # Normalise column names (strip whitespace, lower-case)
        df.columns = [c.strip().lower() for c in df.columns]

        # Rename Spanish column names to English
        rename_map = {
            k: v for k, v in self._EXPECTED_COLUMNS.items() if k in df.columns
        }
        df = df.rename(columns=rename_map)

        missing = set(self._EXPECTED_COLUMNS.values()) - set(df.columns)
        if missing:
            raise ValueError(
                f"indhor.csv is missing columns: {sorted(missing)}. "
                f"Found: {df.columns.tolist()}"
            )

        # Cast to int32 for compact storage
        for col in ("year", "month", "day", "hour", "block"):
            df[col] = df[col].astype("int32")

        self._df = df[["year", "month", "day", "hour", "block"]].reset_index(drop=True)

    def block_hours_map(self) -> Dict[int, List[int]]:
        """Return a dict mapping block_number → sorted list of unique hours-of-day (1-24).

        This aggregates across all days, so it captures the typical daily
        hour assignment for each block.  Note: PLP ``Hora`` is 1-based (1-24).
        """
        if self.is_empty:
            return {}
        assert self._df is not None
        result: Dict[int, List[int]] = {}
        for block, group in self._df.groupby("block"):
            hours = sorted(group["hour"].unique().tolist())
            result[int(block)] = hours
        return result

    def to_dict_list(self) -> List[Dict[str, Any]]:
        """Return the parsed data as a list of row dicts."""
        if self.is_empty:
            return []
        assert self._df is not None
        return self._df.to_dict(orient="records")
