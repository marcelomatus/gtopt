#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting generator cost data to JSON format."""

from typing import Union
from pathlib import Path
from typing import Any, Dict, List
from .base_writer import BaseWriter
from .cost_parser import CostParser
import pandas as pd
import numpy as np


class CostWriter(BaseWriter):
    """Converts cost parser data to JSON format used by GTOPT."""

    def __init__(self, cost_parser: CostParser = None, central_parser=None):
        """Initialize with a CostParser instance."""
        super().__init__(cost_parser)
        self.central_parser = central_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert cost data to JSON array format.

        Returns:
            List of cost dictionaries with:
            - name (str): Generator name
            - stages (list[int]): Stage numbers
            - costs (list[float]): Cost values

        Note:
            Converts numpy arrays to lists for JSON serialization
        """
        if items is None:
            items = self.items
        return [
            {
                "name": cost["name"],
                "stages": cost["stages"].tolist(),
                "costs": cost["costs"].tolist(),
            }
            for cost in items
        ]

    def to_dataframe(self, items=None) -> pd.DataFrame:
        """Convert demand data to pandas DataFrame format.

        Returns:
            DataFrame with:
            - Index: Block numbers (merged from all demands)
            - Columns: Demand values for each bus (column name = bus name)
        """
        if items is None:
            items = self.items

        # Create empty DataFrame to collect all demand series
        df = pd.DataFrame()

        default_cost = {}
        for cost in items:
            if len(cost["stages"]) == 0 or len(cost["values"]) == 0:
                continue

            cname = cost.get("name", "")
            central = self.central_parser.get_central_by_name(cname)
            if central is None:
                continue

            # Create Series for this cost
            id = cost.get("number", cname)
            name = f"uid:{id}" if not isinstance(id, str) else id
            default_cost[name] = central.get("variable_cost", 0.0)

            s = pd.Series(data=cost["values"], index=cost["stages"], name=name)
            # Add to DataFrame
            df = pd.concat([df, s], axis=1)

        if self.stage_parser is not None:
            stages = np.empty(self.stage_parser.num_stages, dtype=np.int16)
            for i, s in enumerate(self.stage_parser.stages):
                stages[i] = s["number"]
            s = pd.Series(data=stages, index=stages, name="stage")
            df = pd.concat([s, df], axis=1)

        # Ensure blocks are sorted and unique
        df = df.sort_index().drop_duplicates()
        # Fill missing values with column-specific defaults
        df = df.fillna(default_cost)

        # Convert index to int16 for memory efficiency
        df.index = df.index.astype("int16")
        # Reset index to make it a column and rename to 'block'
        df = df.reset_index().rename(columns={"index": "stage"})
        # Ensure DataFrame has no duplicate columns
        df = df.loc[:, ~df.columns.duplicated()]
        # Convert cost columns to float64
        cost_cols = [col for col in df.columns if col != "stage"]
        df[cost_cols] = df[cost_cols].astype(np.float64)

        return df

    def to_parquet(self, output_file: Union[str, Path], items=None) -> None:
        """Write demand data to Parquet file format.

        Args:
            output_path: Path to write the Parquet file
            items: Optional list of demand items to convert (uses self.items if None)
        """
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )

        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = output_dir / output_file

        df = self.to_dataframe(items)
        df.to_parquet(output_file, index=True, engine="pyarrow", compression="snappy")
