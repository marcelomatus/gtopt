# -*- coding: utf-8 -*-

"""Writer for converting hydro flow data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict, cast

import numpy as np
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

    def _build_base_columns(
        self, items: list
    ) -> tuple[
        list[str], np.ndarray | None, dict[str, float], list[tuple[str, np.ndarray]]
    ]:
        """Pre-filter items and extract common metadata (once, not per hydrology).

        Returns (col_names, master_index, fill_values, valid_items) where each
        valid_item is ``(col_name, flow_2d_array)`` with flow shape
        ``(num_blocks, num_hydrologies)``.
        """
        if not items:
            return [], None, {}, []

        col_names: list[str] = []
        fill_values: dict[str, float] = {}
        valid_items: list[tuple[str, np.ndarray]] = []
        master_index: np.ndarray | None = None

        for item in items:
            name = item.get("name", "")
            unit = (
                self.central_parser.get_item_by_name(name)
                if self.central_parser
                else None
            )
            if not unit:
                continue
            uid = int(unit["number"]) if "number" in unit else name
            col_name = self.pcol_name(name, uid)

            flow_data = item.get("flow")
            index = item.get("block")
            if flow_data is None or index is None or len(index) == 0:
                continue

            if not isinstance(index, np.ndarray):
                index = np.asarray(index, dtype=np.int32)

            if "afluent" in unit:
                fill_values[col_name] = unit["afluent"]

            if master_index is None:
                master_index = index

            col_names.append(col_name)
            valid_items.append((col_name, flow_data))

        return col_names, master_index, fill_values, valid_items

    def to_dataframe(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> pd.DataFrame:
        """Convert flow data to pandas DataFrame.

        Builds one DataFrame per scenario by slicing the pre-filtered 2D flow
        arrays at the hydrology column, avoiding repeated ``_create_dataframe``
        calls.
        """
        if items is None:
            items = self.items

        if not items:
            return []

        # Collect valid scenarios
        valid_scenarios = []
        for i, scenario in enumerate(self.scenarios):
            hydro_idx = scenario.get("hydrology", i)
            if hydro_idx >= 0:
                valid_scenarios.append((scenario.get("uid", -1), hydro_idx))

        if not valid_scenarios:
            return pd.DataFrame()

        # Pre-filter items once
        _, master_index, fill_values, valid_items = self._build_base_columns(items)
        if not valid_items or master_index is None:
            return pd.DataFrame()

        # Build block index column
        block_col: np.ndarray | None = None
        if self.block_parser and self.block_parser.items:
            block_col = np.array(
                [it["number"] for it in self.block_parser.items], dtype=np.int32
            )

        # Stage mapping (computed once)
        stage_map: dict[int, int] | None = None
        if self.block_parser:
            stage_map = {}
            for blk in master_index:
                stage_map[int(blk)] = self.block_parser.get_stage_number(int(blk))

        # Pre-filter: exclude columns whose values match the fill value
        # across ALL active scenarios.  This must be done globally (not
        # per-scenario) so that every scenario DataFrame has the same
        # columns — otherwise pd.concat introduces NaN, which biases the
        # downstream groupby().mean() in compute_indicators.
        active_hydro_indices = [h for _, h in valid_scenarios]
        filtered_items: list[tuple[str, np.ndarray]] = []
        for col_name, flow_2d in valid_items:
            fv = fill_values.get(col_name)
            if fv is not None and flow_2d.ndim == 2:
                cols = flow_2d[:, active_hydro_indices]
                if np.allclose(cols, fv, rtol=1e-8, atol=1e-11):
                    continue
            filtered_items.append((col_name, flow_2d))

        # Build one DataFrame per scenario by column-slicing the 2D arrays
        dfs: list[pd.DataFrame] = []
        for uid, hydro_idx in valid_scenarios:
            col_data: dict[str, np.ndarray] = {}
            for col_name, flow_2d in filtered_items:
                vals = flow_2d[:, hydro_idx] if flow_2d.ndim == 2 else flow_2d
                col_data[col_name] = vals

            if not col_data:
                continue

            df = pd.DataFrame(col_data, index=master_index)

            # Prepend block index column
            if block_col is not None:
                idx_s = pd.Series(data=block_col, index=block_col, name="block")
                df = pd.concat([idx_s, df], axis=1)

            # Fill missing values
            if fill_values:
                df = df.fillna(fill_values)

            # Add scenario + stage columns
            n = len(df)
            stage_vals = (
                np.array([stage_map.get(int(b), -1) for b in df.index], dtype=np.int32)
                if stage_map
                else np.full(n, -1, dtype=np.int32)
            )
            meta = pd.DataFrame(
                {"scenario": np.full(n, uid, dtype=np.int32), "stage": stage_vals},
                index=df.index,
            )
            dfs.append(pd.concat([meta, df], axis=1))

        if not dfs:
            return pd.DataFrame()

        return pd.concat(dfs, ignore_index=True)

    def to_parquet(self, output_dir: Path, items=None) -> Dict[str, List[str]]:
        """Write flow data to Parquet files (one per hydrology).

        The output file is named ``discharge.parquet`` to match the C++
        ``Flow.discharge`` field name.  The containing directory should be
        ``Flow/`` (matching the C++ ``FlowLP::ClassName``).
        """
        cols: Dict[str, List[str]] = {"discharge": []}
        df = self.to_dataframe(items)
        if not isinstance(df, pd.DataFrame) or df.empty:
            return cols

        output_dir.mkdir(parents=True, exist_ok=True)
        self.write_dataframe(df, output_dir, "discharge")

        cols["discharge"] = df.columns.tolist()
        return cols
