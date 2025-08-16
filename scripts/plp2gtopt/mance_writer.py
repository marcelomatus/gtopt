# -*- coding: utf-8 -*-

"""Writer for converting maintenance data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, TypedDict, cast

import pandas as pd
from .base_writer import BaseWriter
from .mance_parser import ManceParser
from .central_parser import CentralParser
from .block_parser import BlockParser


class GeneratorMaintenance(TypedDict):
    """Represents generator maintenance data."""

    name: str
    block: List[int]
    pmin: List[float]
    pmax: List[float]


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
        super().__init__(mance_parser, options)
        self.central_parser = central_parser
        self.block_parser = block_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format."""
        if items is None:
            items = self.items or []
        json_mances: List[GeneratorMaintenance] = [
            {
                "name": mance["name"],
                "block": mance["block"].tolist(),
                "pmin": mance["pmin"].tolist(),
                "pmax": mance["pmax"].tolist(),
            }
            for mance in items
        ]
        return cast(List[Dict[str, Any]], json_mances)

    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field (pmin/pmax)."""
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
        """Convert maintenance data to pandas DataFrames for pmin and pmax."""
        if items is None:
            items = self.items or []

        if not items:
            return pd.DataFrame(), pd.DataFrame()

        try:
            df_pmin = self._create_dataframe_for_field("pmin", items).copy()
            df_pmax = self._create_dataframe_for_field("pmax", items).copy()
            if self.block_parser:
                df_pmin["stage"] = df_pmin.index.map(
                    self.block_parser.get_stage_number
                ).astype("int32")
                df_pmax["stage"] = df_pmax.index.map(
                    self.block_parser.get_stage_number
                ).astype("int32")

            return df_pmin, df_pmax
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
        """Write maintenance data to Parquet files for pmin and pmax."""
        cols: Dict[str, List[str]] = {"pmin": [], "pmax": []}
        df_pmin, df_pmax = self.to_dataframe(items)

        cols["pmin"] = df_pmin.columns.tolist() if not df_pmin.empty else []
        cols["pmax"] = df_pmax.columns.tolist() if not df_pmax.empty else []

        try:
            # Ensure output directory exists
            output_dir.mkdir(parents=True, exist_ok=True)
            if not df_pmin.empty:
                self._write_parquet_for_field(df_pmin, output_dir / "pmin.parquet")
            if not df_pmax.empty:
                self._write_parquet_for_field(df_pmax, output_dir / "pmax.parquet")
        except Exception as e:
            raise IOError(f"Failed to write Parquet files: {str(e)}") from e
        finally:
            # Explicitly clear and delete DataFrames
            if "df_pmin" in locals():
                del df_pmin
            if "df_pmax" in locals():
                del df_pmax

        return cols
