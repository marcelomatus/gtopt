# -*- coding: utf-8 -*-

"""Writer for converting line maintenance data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional

import pandas as pd
from .base_writer import BaseWriter
from .manli_parser import ManliParser
from .line_parser import LineParser
from .block_parser import BlockParser


class ManliWriter(BaseWriter):
    """Converts line maintenance parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        manli_parser: Optional[ManliParser] = None,
        line_parser: Optional[LineParser] = None,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a ManliParser instance."""
        super().__init__(manli_parser)
        self.line_parser = line_parser
        self.block_parser = block_parser
        self.options = options or {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format."""
        if items is None:
            items = self.items
        return [
            {
                "name": manli["name"],
                "block": manli["block"].tolist(),
                "p_max_ab": manli["p_max_ab"].tolist(),
                "p_max_ba": manli["p_max_ba"].tolist(),
                "operational": manli["operational"].tolist(),
            }
            for manli in items
        ]

    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field."""
        df = self._create_dataframe(
            items=items,
            central_parser=None,  # Not needed for line maintenance
            index_parser=self.block_parser,
            value_field=field,
            index_field="block",
            fill_field=field,
        )
        return df

    def to_dataframe(self, items=None) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
        """Convert maintenance data to pandas DataFrames."""
        if items is None:
            items = self.items

        if not items:
            return pd.DataFrame(), pd.DataFrame(), pd.DataFrame()

        df_pmax_ab = self._create_dataframe_for_field("p_max_ab", items)
        df_pmax_ba = self._create_dataframe_for_field("p_max_ba", items)
        df_operational = self._create_dataframe_for_field("operational", items)

        return df_pmax_ab, df_pmax_ba, df_operational

    def to_parquet(self, output_dir: Path, items=None) -> None:
        """Write maintenance data to Parquet files."""
        df_pmax_ab, df_pmax_ba, df_operational = self.to_dataframe(items)

        try:
            output_dir.mkdir(parents=True, exist_ok=True)
            
            compression = self.options.get("compression", "gzip")
            df_pmax_ab.to_parquet(
                output_dir / "p_max_ab.parquet",
                index=False,
                compression=compression
            )
            df_pmax_ba.to_parquet(
                output_dir / "p_max_ba.parquet",
                index=False,
                compression=compression
            )
            df_operational.to_parquet(
                output_dir / "operational.parquet",
                index=False,
                compression=compression
            )
        finally:
            # Clean up DataFrames
            del df_pmax_ab, df_pmax_ba, df_operational
