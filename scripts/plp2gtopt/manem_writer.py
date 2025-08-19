# -*- coding: utf-8 -*-

"""Writer for converting reservoir maintenance data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, TypedDict, cast

import pandas as pd
from .base_writer import BaseWriter
from .manem_parser import ManemParser
from .central_parser import CentralParser
from .block_parser import BlockParser


class ReservoirMaintenance(TypedDict):
    """Represents reservoir maintenance data."""

    name: str
    block: List[int]
    volmin: List[float]
    volmax: List[float]


class ManemWriter(BaseWriter):
    """Converts reservoir maintenance parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        manem_parser: Optional[ManemParser] = None,
        central_parser: Optional[CentralParser] = None,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a ManemParser instance.

        Args:
            manem_parser: Parser for reservoir maintenance data
            central_parser: Parser for central data
            block_parser: Parser for block data
            options: Dictionary of writer options
        """
        super().__init__(manem_parser, options)
        self.central_parser = central_parser
        self.block_parser = block_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format."""
        if items is None:
            items = self.items or []
        json_manems: List[ReservoirMaintenance] = [
            {
                "name": manem["name"],
                "block": manem["block"].tolist(),
                "volmin": manem["volmin"].tolist(),
                "volmax": manem["volmax"].tolist(),
            }
            for manem in items
        ]
        return cast(List[Dict[str, Any]], json_manems)

    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field (volmin/volmax)."""
        df = self._create_dataframe(
            items=items,
            unit_parser=self.central_parser,
            index_parser=self.block_parser,
            value_field=field,
            index_field="block",
            fill_field=field,
            skip_types=("falla"),
        )

        return df

    def to_dataframe(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> Tuple[pd.DataFrame, pd.DataFrame]:
        """Convert maintenance data to pandas DataFrames for volmin and volmax."""
        if items is None:
            items = self.items or []

        if not items:
            return pd.DataFrame(), pd.DataFrame()

        try:
            df_volmin = self._create_dataframe_for_field("volmin", items).copy()
            df_volmax = self._create_dataframe_for_field("volmax", items).copy()
            if self.block_parser:
                df_volmin["stage"] = df_volmin.index.map(
                    self.block_parser.get_stage_number
                ).astype("int32")
                df_volmax["stage"] = df_volmax.index.map(
                    self.block_parser.get_stage_number
                ).astype("int32")

            return df_volmin, df_volmax
        except Exception as e:
            raise ValueError(f"Failed to create DataFrames: {str(e)}") from e

    def _write_parquet_for_field(self, df: pd.DataFrame, output_path: Path) -> None:
        """Write a single DataFrame to parquet format.

        Args:
            df: DataFrame containing the field data
            output_path: Path to write the parquet file to
        """
        if df.empty:
            return

        df.to_parquet(
            output_path,
            index=False,
            compression=self.get_compression(),
        )

    def to_parquet(self, output_dir: Path, items=None) -> Dict[str, List[str]]:
        """Write maintenance data to Parquet files for volmin and volmax."""
        cols: Dict[str, List[str]] = {"volmin": [], "volmax": []}
        df_volmin, df_volmax = self.to_dataframe(items)

        cols["volmin"] = df_volmin.columns.tolist() if not df_volmin.empty else []
        cols["volmax"] = df_volmax.columns.tolist() if not df_volmax.empty else []

        try:
            # Ensure output directory exists
            output_dir.mkdir(parents=True, exist_ok=True)
            if not df_volmin.empty:
                self._write_parquet_for_field(df_volmin, output_dir / "volmin.parquet")
            if not df_volmax.empty:
                self._write_parquet_for_field(df_volmax, output_dir / "volmax.parquet")
        except Exception as e:
            raise IOError(f"Failed to write Parquet files: {str(e)}") from e
        finally:
            # Explicitly clear and delete DataFrames
            if "df_volmin" in locals():
                del df_volmin
            if "df_volmax" in locals():
                del df_volmax

        return cols
