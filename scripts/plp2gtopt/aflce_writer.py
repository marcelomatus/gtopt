# -*- coding: utf-8 -*-

"""Writer for converting hydro flow data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional
import pandas as pd
import numpy as np

from .base_writer import BaseWriter
from .aflce_parser import AflceParser
from .central_parser import CentralParser
from .block_parser import BlockParser


class AflceWriter(BaseWriter):
    """Converts flow parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        aflce_parser: Optional[AflceParser] = None,
        central_parser: Optional[CentralParser] = None,
        block_parser: Optional[BlockParser] = None,
        scenarios: Optional[List[Dict[str, Any]]] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with an AflceParser instance."""
        super().__init__(aflce_parser)
        self.central_parser = central_parser
        self.block_parser = block_parser
        self.scenarios = scenarios or []
        self.options = options or {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert flow data to JSON array format."""
        if items is None:
            items = self.items
        return [
            {
                "name": flow["name"],
                "blocks": flow["blocks"].tolist(),
                "flows": flow["flows"].tolist(),
            }
            for flow in items
        ]

    def _create_dataframe_for_hydrology(
        self, hydro_idx: int, items: list
    ) -> pd.DataFrame:
        """Create a DataFrame for a specific hydrology."""
        df = self._create_dataframe(
            items=items,
            unit_parser=self.central_parser,
            index_parser=self.block_parser,
            value_field="flows",
            index_field="blocks",
            fill_field="afluent",
            value_oper=lambda v: v[hydro_idx],
        )
        return df

    def to_dataframe(self, items=None) -> List[pd.DataFrame]:
        """Convert flow data to pandas DataFrames (one per hydrology)."""
        if items is None:
            items = self.items

        if not items:
            return []

        dfs = []
        for i, scenario in enumerate(self.scenarios):
            hydro_idx = scenario.get("hydrology", i)
            if hydro_idx < 0 or hydro_idx >= len(self.scenarios):
                continue
            df = self._create_dataframe_for_hydrology(hydro_idx, items)
            if df.empty:
                continue

            df["scenario"] = scenario.get("uid", -1)
            df["scenario"] = df["scenario"].astype('int16')
            if self.block_parser:
                df["stage"] = df.index.map(self.block_parser.get_stage_number).astype("int16")

            dfs.append(df)

        # Combine all DataFrames into one
        return pd.concat(dfs, ignore_index=True) if dfs else pd.DataFrame()

    def to_parquet(self, output_dir: Path, items=None) -> None:
        """Write flow data to Parquet files (one per hydrology)."""
        dfs = self.to_dataframe(items)
        if not dfs:
            return

        output_dir.mkdir(parents=True, exist_ok=True)

        compression = self.options.get("compression", "gzip")
        if compression not in ["gzip", "snappy", "brotli", "none"]:
            raise ValueError(f"Unsupported compression format: {compression}")

        for i, df in enumerate(dfs, 1):
            if df.empty:
                continue
            output_file = output_dir / f"afluent_h{i}.parquet"
            df.to_parquet(output_file, index=False, compression=compression)
