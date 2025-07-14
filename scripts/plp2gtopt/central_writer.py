# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""
import pandas as pd
import numpy as np
from pathlib import Path
from typing import Union, Any, Dict, List

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .cost_parser import CostParser
from .stage_parser import StageParser


class CentralWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: CentralParser = None,
        stage_parser: StageParser = None,
        cost_parser: CostParser = None,
        options: Dict[str, Any] = None,
    ):
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser)
        self.stage_parser = stage_parser
        self.cost_parser = cost_parser
        self.options = options if options is not None else {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = self.items

        json_centrals = []
        for cen in items:
            # Skip centrals without a bus or with bus 0
            if cen.get("bus", -1) <= 0:
                continue

            cost = (
                self.cost_parser.get_cost_by_name(cen["name"])
                if self.cost_parser
                else None
            )

            gcost = cen.get("variable_cost", 0.0) if cost is None else "gcost"

            central = {
                "uid": cen["number"],
                "name": cen["name"],
                "bus": cen["bus"],
                "gcost": gcost,
                "capacity": float(cen.get("p_max", 0)),
                "efficiency": float(cen.get("efficiency", 1.0)),
                "pmax": float(cen.get("p_max", 0.0)),
                "pmin": float(cen.get("p_min", 0.0)),
                "type": cen.get("type", "unknown"),
            }

            json_centrals.append(central)

        self.to_parquet("gcost.parquet")

        return json_centrals

    def to_dataframe(self, cost_items=None) -> pd.DataFrame:
        """Convert demand data to pandas DataFrame format.

        Returns:
            DataFrame with:
            - Index: Block numbers (merged from all demands)
            - Columns: Demand values for each bus (column name = bus name)
        """
        if cost_items is None:
            cost_items = self.cost_parser.get_all() if self.cost_parser else []

        # Create empty DataFrame to collect all demand series
        df = pd.DataFrame()

        if not cost_items:
            return df

        fill_values = {}
        for cost in cost_items:
            cname = cost.get("name", "")
            central = self.parser.get_central_by_name(cname) if self.parser else None
            if central is None:
                continue

            if len(cost["stages"]) == 0 or len(cost["costs"]) == 0:
                continue

            # Create Series for this cost
            id = central.get("number", cname)
            name = f"uid:{id}" if not isinstance(id, str) else id
            fill_values[name] = float(central.get("variable_cost", 0.0))

            s = pd.Series(data=cost["costs"], index=cost["stages"], name=name)
            # Add to DataFrame
            df = pd.concat([df, s], axis=1)

        # Ensure blocks are sorted and unique
        df = df.sort_index().drop_duplicates()
        # Ensure DataFrame has no duplicate columns
        df = df.loc[:, ~df.columns.duplicated()]
        # Convert cost columns to float64
        cost_cols = [col for col in df.columns if col != "stage"]
        df[cost_cols] = df[cost_cols].astype(np.float64)

        # Convert index to stage column
        if self.stage_parser is not None:
            stages = np.empty(self.stage_parser.num_stages, dtype=np.int16)
            for i, s in enumerate(self.stage_parser.stages):
                stages[i] = int(s["number"])
            s = pd.Series(data=stages, index=stages, name="stage")
            df = pd.concat([s, df], axis=1)
        else:
            # convert index to "stage" column
            df = df.reset_index().rename(columns={"index": "stage"})
            df["stage"] = df["stage"].astype("int16")

        # Fill missing values with column-specific defaults
        df = df.fillna(fill_values)

        return df

    def to_parquet(self, output_file: Union[str, Path], cost_items=None) -> None:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )

        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = output_dir / output_file

        df = self.to_dataframe(cost_items)
        if df.empty:
            return

        compression = self.options.get("compression", "zstd")
        df.to_parquet(output_file, index=False, compression=compression)
