# -*- coding: utf-8 -*-
"""Parser for PLP indhor.csv files mapping calendar hours to block numbers.

The file has columns: Año, Mes, Dia, Hora, Bloque (all integers, 1-based Hora
and Bloque).  It records for every calendar hour in the PLP horizon which
PLP block that hour belongs to.  This mapping is used to reconstruct hourly
time-series from block-level gtopt solver output.
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any, Dict, List, Optional

import pandas as pd


class IndhorParser:
    """Parser for indhor.csv files containing the hour-to-block mapping.

    After parsing, ``self.df`` is a :class:`pandas.DataFrame` with
    column names: ``year``, ``month``, ``day``, ``hour``, ``block`` — all ``int32``.
    ``hour`` is kept 1-based (1-24) to match PLP convention.
    """

    _CSV_ENCODINGS = ("utf-8", "latin-1")

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
            ValueError: if required columns are missing or file cannot be decoded.
        """
        if not self.file_path.exists():
            raise FileNotFoundError(f"indhor.csv not found: {self.file_path}")

        df: Optional[pd.DataFrame] = None
        for enc in self._CSV_ENCODINGS:
            try:
                # header=0 tells pandas the first row contains headers and should be replaced.
                # names=[...] explicitly assigns our desired column names.
                df = pd.read_csv(
                    self.file_path,
                    encoding=enc,
                    header=0,
                    names=["year", "month", "day", "hour", "block"],
                )
                break
            except UnicodeDecodeError:
                logging.getLogger(__name__).debug(
                    "indhor.csv: encoding '%s' failed, trying next", enc
                )
                continue
            except ValueError as e:
                # Catches mismatched column counts if the file format is unexpected
                raise ValueError(
                    f"Error parsing {self.file_path} with encoding {enc}: {e}"
                )

        if df is None:
            raise ValueError(
                f"Cannot decode {self.file_path} with any supported encoding"
            )

        # Cast to int32 for compact storage
        for col in ("year", "month", "day", "hour", "block"):
            df[col] = df[col].astype("int32")

        self._df = df.reset_index(drop=True)

    def block_hours_map(self) -> Dict[int, List[int]]:
        """Return a dict mapping block_number → sorted list of unique hours-of-day (1-24).

        This aggregates across all days, so it captures the typical daily
        hour assignment for each block.  Note: PLP ``Hora`` is 1-based (1-24).
        """
        if self._df is None or self._df.empty:
            return {}
        result: Dict[int, List[int]] = {}
        for block, group in self._df.groupby("block"):
            hours = sorted(group["hour"].unique().tolist())
            result[int(block)] = hours
        return result

    def to_dict_list(self) -> List[Dict[str, Any]]:
        """Return the parsed data as a list of row dicts."""
        if self._df is None or self._df.empty:
            return []
        return self._df.to_dict(orient="records")
