# -*- coding: utf-8 -*-

"""Writer for converting generator cost data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional
import pandas as pd

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
        """Convert cost data to pandas DataFrame format."""
        if items is None:
            items = self.items or []

        def filter_fn(cost):
            cname = cost.get("name", "")
            central = (
                self.central_parser.get_central_by_name(cname)
                if self.central_parser
                else None
            )
            return not central or len(cost["stages"]) == 0

        fill_values = {}
        if self.central_parser:
            for cost in items:
                cname = cost.get("name", "")
                central = self.central_parser.get_central_by_name(cname)
                if central:
                    uid = central.get("number", cname)
                    name = f"uid:{uid}" if not isinstance(uid, str) else uid
                    fill_values[name] = float(central.get("variable_cost", 0.0))

        df = self._create_dataframe(
            items=items,
            value_field="costs",
            index_field="stages",
            parser=self.stage_parser,
            index_name="stage",
            filter_fn=filter_fn,
            fill_values=fill_values
        )
        df["stage"] = df["stage"].astype("int16")
        return df

    def to_parquet(self, output_dir: Path, cost_items=None) -> None:
        """Write demand data to Parquet file format."""
        df = self.to_dataframe(cost_items)
        if df.empty:
            return

        output_file = output_dir / "gcost.parquet"
        compression = self.options.get("compression", "zstd")
        df.to_parquet(output_file, index=False, compression=compression)
