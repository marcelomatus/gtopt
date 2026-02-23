# -*- coding: utf-8 -*-

"""Writer for converting hydro flow data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict, cast
import pandas as pd

from .base_writer import BaseWriter
from .aflce_parser import AflceParser
from .central_parser import CentralParser
from .block_parser import BlockParser


class HydroFlow(TypedDict):
    """Represents a hydro flow entry."""

    name: str
    block: List[int]
    flow: List[float]


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
        super().__init__(aflce_parser, options)
        self.central_parser = central_parser
        self.block_parser = block_parser
        self.scenarios = scenarios or []

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert flow data to JSON array format."""
        if items is None:
            items = self.items or []

        json_flows: List[HydroFlow] = [
            {
                "name": flow["name"],
                "block": flow["block"].tolist(),
                "flow": flow["flow"].tolist(),
            }
            for flow in items
        ]
        return cast(List[Dict[str, Any]], json_flows)

    def _create_dataframe_for_hydrology(
        self, hydro_idx: int, items: list
    ) -> pd.DataFrame:
        """Create a DataFrame for a specific hydrology."""
        df = self._create_dataframe(
            items=items,
            unit_parser=self.central_parser,
            index_parser=self.block_parser,
            value_field="flow",
            index_field="block",
            fill_field="afluent",
            value_oper=lambda v: v[hydro_idx],
        )
        return df

    def to_dataframe(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> pd.DataFrame:
        """Convert flow data to pandas DataFrame.

        Args:
            items: Optional list of flow items to convert. Uses self.items if None.

        Returns:
            DataFrame containing flow data with columns:
            - block: Block numbers
            - scenario: Scenario IDs
            - stage: Stage numbers
            - afluent: Flow values

        Raises:
            ValueError: If input data is invalid
        """
        if items is None:
            items = self.items

        if not items:
            return []

        # Build all data at once
        scenario_data = []

        for i, scenario in enumerate(self.scenarios):
            hydro_idx = scenario.get("hydrology", i)
            if hydro_idx < 0:
                continue

            df = self._create_dataframe_for_hydrology(hydro_idx, items)
            if df.empty:
                continue

            scenario_data.append(
                {
                    "df": df,
                    "uid": scenario.get("uid", -1),
                    "stage": (
                        df["block"].map(self.block_parser.get_stage_number)
                        if self.block_parser
                        else None
                    ),
                }
            )

        if not scenario_data:
            return pd.DataFrame()

        # Concatenate all at once
        dfs = [
            pd.concat(
                [
                    pd.DataFrame(
                        {"scenario": data["uid"], "stage": data["stage"]},
                        index=data["df"].index,
                    ).astype({"scenario": "int32", "stage": "int32"}),
                    data["df"],
                ],
                axis=1,
            )
            for data in scenario_data
        ]

        return pd.concat(dfs, ignore_index=True)

    def to_parquet(self, output_dir: Path, items=None) -> Dict[str, List[str]]:
        """Write flow data to Parquet files (one per hydrology)."""
        cols: Dict[str, List[str]] = {"afluent": []}
        df = self.to_dataframe(items)
        if not isinstance(df, pd.DataFrame) or df.empty:
            return cols

        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = output_dir / "afluent.parquet"

        df.to_parquet(
            output_file,
            index=False,  # Don't write row indices to file
            compression=self.get_compression(),
        )

        cols["afluent"] = df.columns.tolist()
        return cols
