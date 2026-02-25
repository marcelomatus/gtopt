# -*- coding: utf-8 -*-

"""Writer for converting demand data to JSON format."""

from typing import Any, Dict, List, Optional, TypedDict, cast
from pathlib import Path
import pandas as pd

from .base_writer import BaseWriter
from .demand_parser import DemandParser
from .block_parser import BlockParser


class Demand(TypedDict, total=False):
    """Represents a demand in the system."""

    uid: int | str
    name: str
    bus: int | str
    lmax: str
    emin: float


class DemandWriter(BaseWriter):
    """Converts demand parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        demand_parser: Optional[DemandParser] = None,
        block_parser: Optional[BlockParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a DemandParser instance."""
        super().__init__(demand_parser, options)
        self.block_parser = block_parser

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert demand data to JSON array format."""
        if items is None:
            items = self.items

        self.to_parquet(items=items)

        total_energy = 0.0
        json_demands: List[Demand] = []
        for demand in items:
            if demand.get("bus", -1) == 0:
                continue

            if len(demand["blocks"]) == 0 or len(demand["values"]) == 0:
                continue

            dem: Demand = {
                "uid": demand.get("bus", demand["number"]),
                "name": demand["name"],
                "bus": demand.get("bus", demand["name"]),
                "lmax": "lmax",
                **({"emin": demand["emin"]} if "emin" in demand else {}),
            }
            total_energy += demand["energy"]

            json_demands.append(dem)

        # print(f"Total Energy Demand [GWh]: {round(total_energy / 1000.0, 2)}")
        return cast(List[Dict[str, Any]], json_demands)

    def to_dataframe(self, items: Optional[List[Dict[str, Any]]] = None):
        """Convert demand data to pandas DataFrame format.

        Returns:
            DataFrame with:
            - Index: Block numbers (merged from all demands)
            - Columns: Demand values for each bus (column name = bus name)
        """
        df, de = [pd.DataFrame(), pd.DataFrame()]

        if items is None:
            items = self.items
        if not items:
            return df, de

        management_factor = self.options.get("management_factor", 0.0)

        blocks = self.block_parser.items if self.block_parser else []
        last_stage = self._get_last_stage(blocks)

        series = []
        energies = []
        for demand in items:
            if demand.get("bus", -1) == 0:
                continue

            if len(demand["blocks"]) == 0 or len(demand["values"]) == 0:
                continue

            # Create Series for this demand
            uid = demand.get("bus", demand["name"])
            name = f"uid:{uid}" if not isinstance(uid, str) else uid

            s = pd.Series(data=demand["values"], index=demand["blocks"], name=name)
            s = s.iloc[~s.index.duplicated(keep="last")]
            #
            es = pd.Series(
                data=demand["energies"],
                index=demand["stages"],
                name=name,
            )
            te = es[:last_stage].sum()
            demand["energy"] = te
            if management_factor > 0.0:
                s = (1.0 - management_factor) * s
                e = management_factor * es
                demand["emin"] = "emin"
                energies.append(e)

            # Add to DataFrame
            series.append(s)

        df = pd.concat(series, axis=1)
        if self.block_parser:
            stage_series = pd.Series(
                df.index.map(self.block_parser.get_stage_number).astype("int32"),
                index=df.index,
                name="stage",
            )
            df = pd.concat([df, stage_series], axis=1)
        # Convert index to block column
        df.index = df.index.astype("int32")
        df = df.copy()  # Defragment to avoid PerformanceWarning on reset_index
        df = df.reset_index().rename(columns={"index": "block"})

        # Fill missing values with column-specific defaults
        fill_values = {
            "stage": -1,  # Special value for missing stages
            **{
                col: 0.0 for col in df.columns if col not in ("stage", "block")
            },  # 0.0 for demands
        }
        df = df.fillna(fill_values)

        # prepare the energies dataframe
        if energies:
            de = pd.concat(energies, axis=1)
            de.index = de.index.astype("int32")
            de = de.reset_index().rename(columns={"index": "stage"})

        return df, de

    def to_parquet(self, items=None) -> None:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Demand"
            if "output_dir" in self.options
            else Path("Demand")
        )

        output_dir.mkdir(parents=True, exist_ok=True)

        df, de = self.to_dataframe(items)

        if not df.empty:
            df.to_parquet(
                output_dir / "lmax.parquet",
                index=False,
                compression=self.get_compression(),
            )

        if not de.empty:
            de.to_parquet(
                output_dir / "emin.parquet",
                index=False,
                compression=self.get_compression(),
            )
