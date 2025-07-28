# -*- coding: utf-8 -*-

"""Writer for converting demand data to JSON format."""

from typing import Any, Dict, List, Union, Optional
from pathlib import Path
import pandas as pd

from .base_writer import BaseWriter
from .demand_parser import DemandParser
from .block_parser import BlockParser


class DemandWriter(BaseWriter):
    """Converts demand parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        demand_parser: Optional[DemandParser] = None,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a DemandParser instance."""
        super().__init__(demand_parser)
        self.block_parser = block_parser
        self.options = options if options is not None else {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert demand data to JSON array format."""
        if items is None:
            items = self.items

        json_demands = []
        for demand in items:
            if demand.get("bus", -1) == 0:
                continue

            if len(demand["blocks"]) == 0 or len(demand["values"]) == 0:
                continue

            dem = {
                "uid": demand.get("bus", demand["number"]),
                "name": demand["name"],
                "bus": demand.get("bus", demand["name"]),
                "lmax": "lmax",
            }

            json_demands.append(dem)

        self.to_parquet("lmax.parquet", items=items)
        return json_demands

    def to_dataframe(self, items=None) -> pd.DataFrame:
        """Convert demand data to pandas DataFrame format.

        Returns:
            DataFrame with:
            - Index: Block numbers (merged from all demands)
            - Columns: Demand values for each bus (column name = bus name)
        """
        if items is None:
            items = self.items

        if not items:
            return pd.DataFrame()

        series = []
        for demand in items:
            if demand.get("bus", -1) == 0:
                continue

            if len(demand["blocks"]) == 0 or len(demand["values"]) == 0:
                continue

            # Create Series for this demand
            uid = demand.get("bus", demand["name"])
            name = f"uid:{uid}" if not isinstance(uid, str) else uid

            s = pd.Series(data=demand["values"], index=demand["blocks"], name=name)
            s = s.iloc[~s.index.duplicated(keep="last")]
            # Add to DataFrame
            series.append(s)

        df = pd.concat(series, axis=1)

        if self.block_parser:
            df["stage"] = df.index.map(
                lambda block_num: self.block_parser.get_stage_number(block_num)
            ).astype("int16")

        # Convert index to block column
        df.index = df.index.astype("int16")
        df = df.reset_index().rename(columns={"index": "block"})

        # Fill missing values with column-specific defaults
        fill_values = {
            "stage": -1,  # Special value for missing stages
            **{
                col: 0.0 for col in df.columns if col not in ("stage", "block")
            },  # 0.0 for demands
        }
        df = df.fillna(fill_values)

        return df

    def to_parquet(self, output_file: Union[str, Path], items=None) -> None:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Demand"
            if "output_dir" in self.options
            else Path("Demand")
        )

        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = output_dir / output_file

        df = self.to_dataframe(items)
        if df.empty:
            return

        df.to_parquet(
            output_file,
            index=False,
            compression=self.get_compression(),
        )
