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
    emin: List[float]
    emax: List[float]


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
                "emin": manem["emin"].tolist(),
                "emax": manem["emax"].tolist(),
            }
            for manem in items
        ]
        return cast(List[Dict[str, Any]], json_manems)

    def _create_dataframe_for_field(self, field: str, items: list) -> pd.DataFrame:
        """Create a DataFrame for a specific maintenance field (emin/emax)."""
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
        """Convert maintenance data to pandas DataFrames for emin and emax."""
        if items is None:
            items = self.items or []

        if not items:
            return pd.DataFrame(), pd.DataFrame()

        try:
            df_emin = self._create_dataframe_for_field("emin", items).copy()
            df_emax = self._create_dataframe_for_field("emax", items).copy()

            return df_emin, df_emax
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
        """Write maintenance data to Parquet files for emin and emax."""
        cols: Dict[str, List[str]] = {"emin": [], "emax": []}
        df_emin, df_emax = self.to_dataframe(items)

        cols["emin"] = df_emin.columns.tolist() if not df_emin.empty else []
        cols["emax"] = df_emax.columns.tolist() if not df_emax.empty else []

        try:
            # Ensure output directory exists
            output_dir.mkdir(parents=True, exist_ok=True)
            if not df_emin.empty:
                self._write_parquet_for_field(df_emin, output_dir / "emin.parquet")
            if not df_emax.empty:
                self._write_parquet_for_field(df_emax, output_dir / "emax.parquet")
        except Exception as e:
            raise IOError(f"Failed to write Parquet files: {str(e)}") from e
        finally:
            # Explicitly clear and delete DataFrames
            if "df_emin" in locals():
                del df_emin
            if "df_emax" in locals():
                del df_emax

        return cols
