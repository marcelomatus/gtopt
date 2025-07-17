# -*- coding: utf-8 -*-

"""Writer for converting maintenance data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np
import pandas as pd
from .base_writer import BaseWriter
from .mance_parser import ManceParser
from .central_parser import CentralParser
from .block_parser import BlockParser


class ManceWriter(BaseWriter):
    """Converts maintenance parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        mance_parser: Optional[ManceParser] = None,
        central_parser: Optional[CentralParser] = None,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a ManceParser instance.

        Args:
            mance_parser: Parser for maintenance data
            central_parser: Parser for central data
            block_parser: Parser for block data
            options: Dictionary of writer options
        """
        super().__init__(mance_parser)
        self.central_parser = central_parser
        self.block_parser = block_parser
        self.options = options or {}

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format."""
        if items is None:
            items = self.items
        return [
            {
                "name": mance["name"],
                "blocks": mance["blocks"].tolist(),
                "p_min": mance["p_min"].tolist(),
                "p_max": mance["p_max"].tolist(),
            }
            for mance in items
        ]


    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field (pmin/pmax)."""
        df = pd.DataFrame()

        if not items:
            return df

        fill_values = {}
        for mance in items:
            cname = mance.get("name", "")
            central = (
                self.central_parser.get_central_by_name(cname)
                if self.central_parser
                else None
            )
            if not central or central["type"] == "falla" or len(mance["blocks"]) == 0:
                continue

            uid = central.get("number", cname)
            name = f"uid:{uid}" if not isinstance(uid, str) else uid
            fill_values[name] = float(central.get(field, 0.0))

            # Skip if all field values match the fill value
            if np.all(mance[field] == fill_values[name]):
                continue

            s = pd.Series(data=mance[field], index=mance["blocks"], name=name)
            s = s.loc[~s.index.duplicated(keep="last")]
            df = pd.concat([df, s], axis=1)

        # Post-processing
        df = df.sort_index().drop_duplicates()

        # Convert index to block column
        df = self._convert_index_to_column(
            df, index_name="block", parser=self.block_parser, item_key="number"
        )

        df = df.fillna(fill_values)

        return df

    def to_dataframe(self, items=None) -> tuple[pd.DataFrame, pd.DataFrame]:
        """Convert maintenance data to pandas DataFrames for pmin and pmax."""
        if items is None:
            items = self.items

        if not items:
            return pd.DataFrame(), pd.DataFrame()

        df_pmin = self._create_dataframe_for_field("p_min", items)
        df_pmax = self._create_dataframe_for_field("p_max", items)

        return df_pmin, df_pmax

    def _write_parquet_for_field(self, df: pd.DataFrame, output_path: Path) -> None:
        """Write a single DataFrame to parquet format."""
        if df.empty:
            return

        compression = self.options.get("compression", "gzip")
        df.to_parquet(
            output_path, index=False, engine="pyarrow", compression=compression
        )

    def to_parquet(self, output_dir: Path, items=None) -> None:
        """Write maintenance data to Parquet files for pmin and pmax."""
        df_pmin, df_pmax = self.to_dataframe(items)

        try:
            self._write_parquet_for_field(df_pmin, output_dir / "pmin.parquet")
            self._write_parquet_for_field(df_pmax, output_dir / "pmax.parquet")
        finally:
            # Explicitly clear and delete DataFrames
            if "df_pmin" in locals():
                df_pmin = None
                del df_pmin
            if "df_pmax" in locals():
                df_pmax = None
                del df_pmax
