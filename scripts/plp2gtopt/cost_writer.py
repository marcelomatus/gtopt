# -*- coding: utf-8 -*-

"""Writer for converting generator cost data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional
import pandas as pd

import numpy as np
from .base_writer import BaseWriter
from .cost_parser import CostParser
from .central_parser import CentralParser
from .stage_parser import StageParser


class CostWriter(BaseWriter):
    """Converts cost parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        cost_parser: Optional[CostParser] = None,
        central_parser: Optional[CentralParser] = None,
        stage_parser: Optional[StageParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a CostParser instance."""
        super().__init__(cost_parser)
        self.central_parser = central_parser
        self.stage_parser = stage_parser
        self.options = options or {}

    def _convert_index_to_column(
        self, 
        df: pd.DataFrame, 
        index_name: str,
        parser: Optional[Any] = None,
        num_items: Optional[int] = None,
        items: Optional[List[Dict]] = None,
        item_key: str = "number"
    ) -> pd.DataFrame:
        """Convert DataFrame index to a named column using parser data if available.
        
        Args:
            df: Input DataFrame
            index_name: Name for the new column (e.g. "block" or "stage")
            parser: Optional parser object containing items data
            num_items: Number of items if parser is available
            items: List of item dictionaries if parser is available
            item_key: Key to extract from item dictionaries
            
        Returns:
            DataFrame with index converted to column
        """
        if parser and hasattr(parser, f"num_{index_name}s"):
            num_items = getattr(parser, f"num_{index_name}s")
            items = getattr(parser, f"{index_name}s")
        
        if parser and num_items and items:
            index_values = np.empty(num_items, dtype=np.int16)
            for i, item in enumerate(items):
                index_values[i] = int(item[item_key])
            s = pd.Series(data=index_values, index=index_values, name=index_name)
            df = pd.concat([s, df], axis=1)
        else:
            df = df.reset_index().rename(columns={"index": index_name})
        
        return df

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert cost data to JSON array format."""
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
        """Convert demand data to pandas DataFrame format."""
        if items is None:
            items = self.items

        # Create empty DataFrame to collect all demand series
        df = pd.DataFrame()

        if not items:
            return df

        fill_values = {}
        for cost in items:
            cname = cost.get("name", "")
            central = (
                self.central_parser.get_central_by_name(cname)
                if self.central_parser
                else None
            )
            if not central or len(cost["stages"]) == 0:
                continue

            uid = central.get("number", cname)
            name = f"uid:{uid}" if not isinstance(uid, str) else uid
            fill_values[name] = float(central.get("variable_cost", 0.0))

            # Add to DataFrame
            s = pd.Series(data=cost["costs"], index=cost["stages"], name=name)
            df = pd.concat([df, s], axis=1)

        # Ensure blocks are sorted and unique
        df = df.sort_index().drop_duplicates()

        # Convert index to stage column
        df = self._convert_index_to_column(
            df,
            index_name="stage", 
            parser=self.stage_parser,
            item_key="number"
        )
        df["stage"] = df["stage"].astype("int16")

        # Fill missing values with column-specific defaults
        df = df.fillna(fill_values)

        return df

    def to_parquet(self, output_dir: Path, cost_items=None) -> None:
        """Write demand data to Parquet file format."""
        df = self.to_dataframe(cost_items)
        if df.empty:
            return

        output_file = output_dir / "gcost.parquet"
        compression = self.options.get("compression", "zstd")
        df.to_parquet(output_file, index=False, compression=compression)
