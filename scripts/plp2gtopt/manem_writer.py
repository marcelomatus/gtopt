# -*- coding: utf-8 -*-

"""Writer for converting reservoir maintenance data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, TypedDict, cast

import pandas as pd
from .base_writer import BaseWriter
from .manem_parser import ManemParser
from .central_parser import CentralParser
from .stage_parser import StageParser


class ReservoirMaintenance(TypedDict):
    """Represents reservoir maintenance data."""

    name: str
    stage: List[int]
    vmin: List[float]
    vmax: List[float]


class ManemWriter(BaseWriter):
    """Converts reservoir maintenance parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        manem_parser: Optional[ManemParser] = None,
        central_parser: Optional[CentralParser] = None,
        stage_parser: Optional[StageParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a ManemParser instance.

        Args:
            manem_parser: Parser for reservoir maintenance data
            central_parser: Parser for central data
            stage_parser: Parser for stage data
            options: Dictionary of writer options
        """
        super().__init__(manem_parser, options)
        self.central_parser = central_parser
        self.stage_parser = stage_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert maintenance data to JSON array format."""
        if items is None:
            items = self.items or []
        json_manems: List[ReservoirMaintenance] = [
            {
                "name": manem["name"],
                "stage": manem["stage"].tolist(),
                "vmin": manem["vmin"].tolist(),
                "vmax": manem["vmax"].tolist(),
            }
            for manem in items
        ]
        return cast(List[Dict[str, Any]], json_manems)

    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field (vmin/vmax)."""
        df = self._create_dataframe(
            items=items,
            unit_parser=self.central_parser,
            index_parser=self.stage_parser,
            value_field=field,
            index_field="stage",
            fill_field=field,
            skip_types=("falla"),
        )

        return df

    def to_dataframe(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> Tuple[pd.DataFrame, pd.DataFrame]:
        """Convert maintenance data to pandas DataFrames for vmin and vmax."""
        if items is None:
            items = self.items or []

        if not items:
            return pd.DataFrame(), pd.DataFrame()

        try:
            df_vmin = self._create_dataframe_for_field("vmin", items).copy()
            df_vmax = self._create_dataframe_for_field("vmax", items).copy()

            return df_vmin, df_vmax
        except Exception as e:
            raise ValueError(f"Failed to create DataFrames: {str(e)}") from e

    def _write_parquet_for_field(self, df: pd.DataFrame, output_path: Path) -> None:
        """Write a single DataFrame to parquet format.

        Args:
            df: DataFrame containing the field dataelm
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
        """Write maintenance data to Parquet files for vmin and vmax."""
        cols: Dict[str, List[str]] = {"vmin": [], "vmax": []}
        df_vmin, df_vmax = self.to_dataframe(items)

        cols["vmin"] = df_vmin.columns.tolist() if not df_vmin.empty else []
        cols["vmax"] = df_vmax.columns.tolist() if not df_vmax.empty else []

        try:
            # Ensure output directory exists
            output_dir.mkdir(parents=True, exist_ok=True)
            if not df_vmin.empty:
                self._write_parquet_for_field(df_vmin, output_dir / "vmin.parquet")
            if not df_vmax.empty:
                self._write_parquet_for_field(df_vmax, output_dir / "vmax.parquet")
        except Exception as e:
            raise IOError(f"Failed to write Parquet files: {str(e)}") from e
        finally:
            # Explicitly clear and delete DataFrames
            if "df_vmin" in locals():
                del df_vmin
            if "df_vmax" in locals():
                del df_vmax

        return cols
