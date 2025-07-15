# -*- coding: utf-8 -*-

"""Writer for converting maintenance data to JSON format."""

from typing import Union, Any, Dict, List
from pathlib import Path
from .base_writer import BaseWriter
from .mance_parser import ManceParser
import pandas as pd
import numpy as np


class ManceWriter(BaseWriter):
    """Converts maintenance parser data to JSON format used by GTOPT."""

    def __init__(self, mance_parser: ManceParser = None):
        """Initialize with a ManceParser instance."""
        super().__init__(mance_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format.

        Returns:
            List of maintenance dictionaries with:
            - name (str): Generator name
            - months (list[int]): Month numbers
            - blocks (list[int]): Block numbers
            - p_min (list[float]): Minimum power values
            - p_max (list[float]): Maximum power values
        """
        if items is None:
            items = self.items
        return [
            {
                "name": maint["name"],
                "months": maint["months"].tolist(),
                "blocks": maint["blocks"].tolist(),
                "p_min": maint["p_min"].tolist(),
                "p_max": maint["p_max"].tolist(),
            }
            for maint in items
        ]

    def to_dataframe(self, items=None) -> pd.DataFrame:
        """Convert maintenance data to pandas DataFrame format.

        Returns:
            DataFrame with:
            - Index: Block numbers
            - Columns: Maintenance data for each central
        """
        if items is None:
            items = self.items

        df = pd.DataFrame()

        for maint in items:
            if len(maint["blocks"]) == 0:
                continue

            # Create DataFrame for this maintenance entry
            temp_df = pd.DataFrame({
                "month": maint["months"],
                "block": maint["blocks"],
                f"{maint['name']}_p_min": maint["p_min"],
                f"{maint['name']}_p_max": maint["p_max"]
            })
            df = pd.concat([df, temp_df], ignore_index=True)

        # Fill missing values with 0 (no maintenance)
        df = df.fillna(0)
        # Convert to efficient dtypes
        df["month"] = df["month"].astype("int8")
        df["block"] = df["block"].astype("int16")
        for col in df.columns:
            if "_p_" in col:
                df[col] = df[col].astype("float32")

        return df

    def to_parquet(self, output_file: Union[str, Path], items=None) -> None:
        """Write maintenance data to Parquet file format."""
        df = self.to_dataframe(items)
        df.to_parquet(
            output_file,
            index=False,
            engine="pyarrow",
            compression="snappy"
        )
