# -*- coding: utf-8 -*-

"""Writer for converting line maintenance data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict, cast

import pandas as pd
from .base_writer import BaseWriter
from .manli_parser import ManliParser
from .line_parser import LineParser
from .block_parser import BlockParser


class LineMaintenance(TypedDict):
    """Represents line maintenance data."""

    name: str
    block: List[int]
    tmax_ab: List[float]
    tmax_ba: List[float]
    active: List[int]


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
        super().__init__(manli_parser, options)
        self.line_parser = line_parser
        self.block_parser = block_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format."""
        if items is None:
            items = self.items or []
        json_manlis: List[LineMaintenance] = [
            {
                "name": manli["name"],
                "block": manli["block"].tolist(),
                "tmax_ab": manli["tmax_ab"].tolist(),
                "tmax_ba": manli["tmax_ba"].tolist(),
                "active": manli["operational"].tolist(),
            }
            for manli in items
        ]
        return cast(List[Dict[str, Any]], json_manlis)

    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field."""
        df = self._create_dataframe(
            items=items,
            unit_parser=self.line_parser,
            index_parser=self.block_parser,
            value_field=field,
            index_field="block",
            fill_field=field,
        )
        return df

    def block_to_stage_df(self, df):
        """Process a DataFrame to set stage as index and remove duplicates."""
        if df.empty:
            return df
        # Drop the 'block' column if it exists (it's the index, so reset index first)
        df = df.reset_index()
        if "block" in df.columns:
            df = df.drop(columns=["block"])
        # Set 'stage' as index and remove duplicates
        if "stage" in df.columns:
            df = df.set_index("stage")
            # Remove duplicate index values by keeping the first occurrence
            df = df[~df.index.duplicated(keep="first")]
        return df

    def to_dataframe(
        self, items=None
    ) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
        """Convert maintenance data to pandas DataFrames."""
        if items is None:
            items = self.items

        if not items:
            return pd.DataFrame(), pd.DataFrame(), pd.DataFrame()

        df_tmax_ab = self._create_dataframe_for_field("tmax_ab", items)
        df_tmax_ba = self._create_dataframe_for_field("tmax_ba", items)
        df_active = self._create_dataframe_for_field("operational", items)
        # Add stage column using block_parser if available
        if self.block_parser:
            # Add stage column
            for df in [df_tmax_ab, df_tmax_ba, df_active]:
                if not df.empty:
                    df["stage"] = df.index.map(
                        self.block_parser.get_stage_number
                    ).astype("int32")

        df_active = self.block_to_stage_df(df_active)

        return df_tmax_ab, df_tmax_ba, df_active

    def to_parquet(self, output_dir: Path, items=None) -> Dict[str, List[str]]:
        """Write maintenance data to Parquet files."""
        cols: Dict[str, List[str]] = {"tmax_ab": [], "tmax_ba": [], "active": []}
        df_tmax_ab, df_tmax_ba, df_active = self.to_dataframe(items)

        cols["tmax_ab"] = df_tmax_ab.columns.tolist() if not df_tmax_ab.empty else []
        cols["tmax_ba"] = df_tmax_ba.columns.tolist() if not df_tmax_ba.empty else []
        cols["active"] = df_active.columns.tolist() if not df_active.empty else []

        try:
            output_dir.mkdir(parents=True, exist_ok=True)

            compression = self.get_compression()
            df_tmax_ab.to_parquet(
                output_dir / "tmax_ab.parquet", index=False, compression=compression
            )
            df_tmax_ba.to_parquet(
                output_dir / "tmax_ba.parquet", index=False, compression=compression
            )
            df_active.to_parquet(
                output_dir / "active.parquet", index=False, compression=compression
            )
        finally:
            # Clean up DataFrames
            del df_tmax_ab, df_tmax_ba, df_active

        return cols
