# -*- coding: utf-8 -*-

"""Writer for converting demand data to JSON format."""

from typing import Any, Dict, List, Union
from pathlib import Path
import pandas as pd
from .base_writer import BaseWriter
from .demand_parser import DemandParser
from .block_parser import BlockParser
import numpy as np


class DemandWriter(BaseWriter):
    """Converts demand parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        demand_parser: DemandParser = None,
        block_parser: BlockParser = None,
        options: Dict[str, Any] = None,
    ):
        """Initialize with a DemandParser instance."""
        super().__init__(demand_parser)
        self.block_parser = block_parser
        self.options = options if options is not None else {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert demand data to JSON array format.

        Returns:
            List of demand dictionaries with:
            - uid (int): Bus number
            - name (str): Bus name
            - bus (str): Bus name (same as name)
            - blocks (list[int]): Block numbers
            - values (list[float]): Demand values

        Note:
            Converts numpy arrays to lists for JSON serialization
        """
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

        # Create empty DataFrame to collect all demand series
        df = pd.DataFrame()

        for demand in items:
            if demand.get("bus", -1) == 0:
                continue

            if len(demand["blocks"]) == 0 or len(demand["values"]) == 0:
                continue

            # Create Series for this demand
            bus = demand.get("bus", demand["name"])
            name = f"uid:{bus}" if not isinstance(bus, str) else bus

            s = pd.Series(data=demand["values"], index=demand["blocks"], name=name)
            # Add to DataFrame
            df = pd.concat([df, s], axis=1)

        if self.block_parser is not None:
            stages = np.empty(len(df.index), dtype=np.int16)
            for i in range(len(stages)):
                block_num = int(df.index[i])
                stages[i] = self.block_parser.get_stage_num(block_num)
            s = pd.Series(data=stages, index=df.index, name="stage")
            df = pd.concat([s, df], axis=1)

        # Ensure blocks are sorted and unique
        df = df.sort_index().drop_duplicates()
        # Fill NaN values with 0.0
        df = df.fillna(0.0)
        # Convert index to int16 for memory efficiency
        df.index = df.index.astype("int16")
        # Ensure DataFrame has no duplicate columns
        df = df.loc[:, ~df.columns.duplicated()]
        # Reset index to make it a column and rename to 'block'
        df = df.reset_index().rename(columns={"index": "block"})

        return df

    def to_parquet(self, output_file: Union[str, Path], items=None) -> None:
        """Write demand data to Parquet file format.

        Args:
            output_path: Path to write the Parquet file
            items: Optional list of demand items to convert (uses self.items if None)
        """
        output_dir = (
            self.options["output_dir"] / "Demand"
            if "output_dir" in self.options
            else Path("Demand")
        )

        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = output_dir / output_file

        df = self.to_dataframe(items)
        df.to_parquet(output_file, index=True, engine="pyarrow", compression="snappy")


if __name__ == "__main__":
    BaseWriter.main(DemandWriter, DemandParser)
