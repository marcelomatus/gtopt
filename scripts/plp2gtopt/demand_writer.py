# -*- coding: utf-8 -*-

"""Writer for converting demand data to JSON format."""

import logging
from typing import Any, Dict, List, Optional, TypedDict, cast
from pathlib import Path
import pandas as pd

from .base_writer import BaseWriter
from .demand_parser import DemandParser
from .block_parser import BlockParser

_logger = logging.getLogger(__name__)


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
            self.write_dataframe(df, output_dir, "lmax")

        if not de.empty:
            self.write_dataframe(de, output_dir, "emin")

    def to_fcost_dataframe(
        self,
        demand_array: List[Dict[str, Any]],
        falla_by_bus: Dict[int, Dict[str, Any]],
        cost_parser: Any,
        stage_parser: Any,
        central_parser: Any,
    ) -> tuple[pd.DataFrame, set[Any]]:
        """Build the fcost DataFrame from falla cost schedules.

        When multiple falla centrals sit on the same bus and have cost
        schedules, the **minimum** cost per stage across all of them is
        used.  The base gcost from plpcnfce.dat fills stages not covered
        by the schedule.

        Returns ``(df, filed_buses)`` where *df* has columns
        ``stage, uid:<N>, …`` and *filed_buses* is the set of demand bus
        numbers that got a time-varying fcost.
        """
        empty: tuple[pd.DataFrame, set[Any]] = (pd.DataFrame(), set())

        if not cost_parser or not stage_parser or not central_parser:
            return empty

        # Build demand uid → bus mapping
        bus_to_uid: Dict[int, Any] = {}
        for dem in demand_array:
            bus = dem.get("bus")
            if bus in falla_by_bus:
                bus_to_uid[bus] = dem.get("uid", bus)

        if not bus_to_uid:
            return empty

        # Group all falla centrals by bus
        fallas_per_bus: Dict[int, List[Dict[str, Any]]] = {}
        for central in central_parser.centrals:
            if central.get("type") != "falla":
                continue
            bus = central.get("bus", 0)
            if bus <= 0 or bus not in bus_to_uid:
                continue
            fallas_per_bus.setdefault(bus, []).append(central)

        stages = [s["number"] for s in stage_parser.items]

        series_list: List[pd.Series] = []
        filed_buses: set[Any] = set()

        for bus, fallas in fallas_per_bus.items():
            # Collect cost schedules from all fallas on this bus
            bus_schedules: List[pd.Series] = []
            min_base_gcost = min(f.get("gcost", 0.0) for f in fallas)
            for falla in fallas:
                cost_entry = cost_parser.get_cost_by_name(falla.get("name", ""))
                if cost_entry is None:
                    # No schedule — use constant base gcost
                    bus_schedules.append(
                        pd.Series(
                            falla.get("gcost", 0.0),
                            index=stages,
                            dtype="float64",
                        )
                    )
                else:
                    s = pd.Series(
                        data=cost_entry["cost"],
                        index=cost_entry["stage"].tolist(),
                        dtype="float64",
                    )
                    s = s[~s.index.duplicated(keep="last")]
                    # Reindex to all stages, fill with base gcost
                    s = s.reindex(stages).fillna(falla.get("gcost", 0.0))
                    bus_schedules.append(s)

            if not bus_schedules:
                continue

            # Element-wise minimum across all fallas on this bus
            min_series = bus_schedules[0]
            for s in bus_schedules[1:]:
                min_series = min_series.combine(s, min)

            # Skip writing if the schedule is constant and equals min base gcost
            if (min_series == min_base_gcost).all():
                continue

            dem_uid = bus_to_uid[bus]
            min_series.name = f"uid:{dem_uid}"
            series_list.append(min_series)
            filed_buses.add(bus)

        if not series_list:
            return empty

        df = pd.concat(series_list, axis=1)
        df = df.reset_index().rename(columns={"index": "stage"})
        df["stage"] = df["stage"].astype("int32")

        return df, filed_buses

    def write_fcost(
        self,
        demand_array: List[Dict[str, Any]],
        falla_by_bus: Dict[int, Dict[str, Any]],
        cost_parser: Any,
        stage_parser: Any,
        central_parser: Any,
    ) -> set[Any]:
        """Write Demand/fcost from falla cost schedules.

        Returns the set of demand bus numbers that got a file-based fcost.
        """
        df, filed_buses = self.to_fcost_dataframe(
            demand_array, falla_by_bus, cost_parser, stage_parser, central_parser
        )

        if not df.empty:
            output_dir = self.options.get("output_dir", Path("results")) / "Demand"
            output_dir.mkdir(parents=True, exist_ok=True)
            self.write_dataframe(df, output_dir, "fcost")

        return filed_buses
