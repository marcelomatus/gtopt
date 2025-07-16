# -*- coding: utf-8 -*-

"""Writer for converting generator cost data to JSON format."""

from typing import Union
from pathlib import Path
from typing import Any, Dict, List
from .base_writer import BaseWriter
from .cost_parser import CostParser
from .central_parser import CentralParser
from .stage_parser import StageParser
import pandas as pd
import numpy as np


class CostWriter(BaseWriter):
    """Converts cost parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        cost_parser: CostParser = None,
        central_parser: CentralParser = None,
        stage_parser: StageParser = None,
    ):
        """Initialize with a CostParser instance."""
        super().__init__(cost_parser)
        self.central_parser = central_parser
        self.stage_parser = stage_parser

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

    def to_dataframe(self, cost_items=None) -> pd.DataFrame:
        """Convert demand data to pandas DataFrame format."""
        if cost_items is None:
            cost_items = self.get_all()

        # Create empty DataFrame to collect all demand series
        df = pd.DataFrame()

        if not cost_items:
            return df

        fill_values = {}
        for cost in cost_items:
            cname = cost.get("name", "")
            central = (
                self.central_parser.get_central_by_name(cname)
                if self.central_parser
                else None
            )
            if not central or len(cost["stages"]) == 0:
                continue

            # Create Series for this cost
            id = central.get("number", cname)
            name = f"uid:{id}" if not isinstance(id, str) else id
            fill_values[name] = float(central.get("variable_cost", 0.0))

            # Add to DataFrame
            s = pd.Series(data=cost["costs"], index=cost["stages"], name=name)
            df = pd.concat([df, s], axis=1)

        # Ensure blocks are sorted and unique
        df = df.sort_index().drop_duplicates()

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
