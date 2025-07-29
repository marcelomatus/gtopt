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
                "stage": cost["stage"].tolist(),
                "cost": cost["cost"].tolist(),
            }
            for cost in items
        ]

    def to_dataframe(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> pd.DataFrame:
        """Convert cost data to pandas DataFrame format."""
        if items is None:
            items = self.items or []

        df = self._create_dataframe(
            items=items,
            unit_parser=self.central_parser,
            index_parser=self.stage_parser,
            value_field="cost",
            index_field="stage",
            fill_field="gcost",
        )

        return df

    def to_parquet(self, output_dir: Path, cost_items=None) -> None:
        """Write demand data to Parquet file format."""
        df = self.to_dataframe(cost_items)
        if df.empty:
            return

        output_file = output_dir / "gcost.parquet"

        df.to_parquet(
            output_file,
            index=False,
            compression=self.get_compression(),
        )
