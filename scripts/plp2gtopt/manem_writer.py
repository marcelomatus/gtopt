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
        """Create a DataFrame for a specific maintenance field (emin/emax).

        The emin/emax field must be DENSE — every stage of every reservoir
        with maintenance data gets a value.  ``_create_dataframe`` already
        fills stages NOT listed in the manem entry with the reservoir's BASE
        ``emin`` / ``emax`` (the ``fill_field`` default sourced from
        ``central_parser``).  But when a reservoir's CentralParser record is
        missing the base ``emin`` / ``emax`` field, the per-column fill
        default is absent and the uncovered stages come back as ``NaN`` — a
        NaN bound would corrupt the C++ ``Reservoir`` reader (it broadcasts a
        stage-indexed series across all blocks).  Guard against that here by
        re-filling any residual NaN from the reservoir's base value
        (falling back to 0.0 only when the base itself is unknown), so the
        emitted profile is always complete.
        """
        df = self._create_dataframe(
            items=items,
            unit_parser=self.central_parser,
            index_parser=self.stage_parser,
            value_field=field,
            index_field="stage",
            fill_field=field,
            skip_types=("falla"),
        )

        # Dense-default guard: re-fill any remaining NaN (reservoir whose
        # CentralParser record lacks the base ``field``) with the per-column
        # base value, then 0.0 as a last resort.  ``stage`` (the index
        # column) is never NaN, so it is unaffected.
        if not df.empty:
            data_cols = [c for c in df.columns if c != "stage"]
            for col in data_cols:
                if df[col].isna().any():
                    base = self._base_value_for_column(field, col)
                    df[col] = df[col].fillna(base)

        return df

    def _base_value_for_column(self, field: str, col_name: str) -> float:
        """Return the reservoir BASE ``emin``/``emax`` for a data column.

        Columns are labelled ``uid:<N>`` (or the reservoir name).  Resolve
        back to the CentralParser record and read its base ``field`` value;
        fall back to 0.0 when the central record or field is unavailable.
        """
        if self.central_parser is None:
            return 0.0
        label = col_name.split(":", 1)[1] if col_name.startswith("uid:") else col_name
        unit = None
        # Try numeric uid first, then name.
        try:
            number = int(label)
        except ValueError:
            number = None
        if number is not None and hasattr(self.central_parser, "get_item_by_number"):
            try:
                unit = self.central_parser.get_item_by_number(number)
            except (KeyError, IndexError, ValueError):
                unit = None
        if unit is None and hasattr(self.central_parser, "get_item_by_name"):
            try:
                unit = self.central_parser.get_item_by_name(label)
            except (KeyError, IndexError, ValueError):
                unit = None
        if unit and field in unit:
            try:
                return float(unit[field])
            except (TypeError, ValueError):
                return 0.0
        return 0.0

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

    def to_parquet(self, output_dir: Path, items=None) -> Dict[str, List[str]]:
        """Write maintenance data to Parquet/CSV files for emin and emax."""
        cols: Dict[str, List[str]] = {"emin": [], "emax": []}
        df_emin, df_emax = self.to_dataframe(items)

        cols["emin"] = df_emin.columns.tolist() if not df_emin.empty else []
        cols["emax"] = df_emax.columns.tolist() if not df_emax.empty else []

        output_dir.mkdir(parents=True, exist_ok=True)
        if not df_emin.empty:
            self.write_dataframe(df_emin, output_dir, "emin")
        if not df_emax.empty:
            self.write_dataframe(df_emax, output_dir, "emax")

        return cols
