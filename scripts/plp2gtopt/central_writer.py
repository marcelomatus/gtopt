# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""
import pandas as pd
import numpy as np
from pathlib import Path
from typing import Union, Any, Dict, List

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .cost_parser import CostParser
from .stage_parser import StageParser
from .bus_parser import BusParser
from .mance_parser import ManceParser


class CentralWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: CentralParser = None,
        stage_parser: StageParser = None,
        cost_parser: CostParser = None,
        bus_parser: BusParser = None,
        mance_parser: ManceParser = None,
        options: Dict[str, Any] = None,
    ):
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser)
        self.stage_parser = stage_parser
        self.cost_parser = cost_parser
        self.bus_parser = bus_parser
        self.mance_parser = mance_parser
        self.options = options if options is not None else {}

    def process_central_embalses(self, embalses):
        """Process embalses to include block and stage information."""
        if not embalses:
            return
        pass

    def process_central_series(self, series):
        """Process series to include block and stage information."""
        if not series:
            return
        pass

    def process_central_pasadas(self, pasadas):
        """Process pasadas to include block and stage information."""
        if not pasadas:
            return
        pass

    def process_central_baterias(self, baterias):
        """Process baterias to include block and stage information."""
        if not baterias:
            return
        pass

    def process_central_termicas(self, termicas):
        """Process termicas to include block and stage information."""
        if not termicas:
            return
        pass

    def process_central_fallas(self, fallas):
        """Process fallas to include block and stage information."""
        if not fallas:
            return
        pass

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = self.items
        if not items:
            return []

        self.centrals_of_type = {
            "embalse": [],
            "serie": [],
            "pasada": [],
            "termica": [],
            "bateria": [],
            "falla": [],
        }

        for cen in items:
            self.centrals_of_type[cen["type"]].append(cen)

        self.process_central_embalses(self.centrals_of_type["embalse"])
        self.process_central_series(self.centrals_of_type["serie"])
        self.process_central_pasadas(self.centrals_of_type["pasada"])
        self.process_central_baterias(self.centrals_of_type["bateria"])
        self.process_central_termicas(self.centrals_of_type["termica"])
        self.process_central_fallas(self.centrals_of_type["falla"])

        json_centrals = []
        for cen in items:
            # skip centrals that are "falla" type
            if cen["type"] == "falla":
                # Falla centrals are not included in the output
                continue

            # Skip centrals without a bus or with bus 0
            bus_number = cen.get("bus", -1)
            if bus_number <= 0:
                continue

            if self.bus_parser:
                bus = self.bus_parser.get_bus_by_number(bus_number)
                if bus is None or bus.get("number", -1) <= 0:
                    print(
                        f"Skipping central {cen['name']} with invalid bus {bus_number}."
                    )
                    continue

            # lookup cost by name if cost_parser is available, and use it
            cost = (
                self.cost_parser.get_cost_by_name(cen["name"])
                if self.cost_parser
                else None
            )
            gcost = cen.get("variable_cost", 0.0) if cost is None else "gcost"

            # lookup mance by name if cost_parser is available, and use it
            mance = (
                self.mance_parser.get_mance_by_name(cen["name"])
                if self.mance_parser
                else None
            )
            pmin, pmax = (
                (cen.get("p_min", 0.0), cen.get("p_max", 0.0)) if mance is None else ("pmin", "pmax")

            central = {
                "uid": cen["number"],
                "name": cen["name"],
                "bus": cen["bus"],
                "gcost": gcost,
                "capacity": float(cen.get("p_max", 0)),
                "efficiency": float(cen.get("efficiency", 1.0)),
                "pmax": pmax,
                "pmin": pmin,
                "type": cen.get("type", "unknown"),
            }

            json_centrals.append(central)

        self.to_parquet("gcost.parquet")

        return json_centrals

    def to_dataframe(self, cost_items=None) -> pd.DataFrame:
        """Convert demand data to pandas DataFrame format.

        Returns:
            DataFrame with:
            - Index: Block numbers (merged from all demands)
            - Columns: Demand values for each bus (column name = bus name)
        """
        if cost_items is None:
            cost_items = self.cost_parser.get_all() if self.cost_parser else []

        # Create empty DataFrame to collect all demand series
        df = pd.DataFrame()

        if not cost_items:
            return df

        fill_values = {}
        for cost in cost_items:
            cname = cost.get("name", "")
            central = self.parser.get_central_by_name(cname) if self.parser else None
            if central is None:
                continue

            if len(cost["stages"]) == 0 or len(cost["costs"]) == 0:
                continue

            # Create Series for this cost
            id = central.get("number", cname)
            name = f"uid:{id}" if not isinstance(id, str) else id
            fill_values[name] = float(central.get("variable_cost", 0.0))

            s = pd.Series(data=cost["costs"], index=cost["stages"], name=name)
            # Add to DataFrame
            df = pd.concat([df, s], axis=1)

        # Ensure blocks are sorted and unique
        df = df.sort_index().drop_duplicates()
        # Ensure DataFrame has no duplicate columns
        df = df.loc[:, ~df.columns.duplicated()]
        # Convert cost columns to float64
        cost_cols = [col for col in df.columns if col != "stage"]
        df[cost_cols] = df[cost_cols].astype(np.float64)

        # Convert index to stage column
        if self.stage_parser is not None:
            stages = np.empty(self.stage_parser.num_stages, dtype=np.int16)
            for i, s in enumerate(self.stage_parser.stages):
                stages[i] = int(s["number"])
            s = pd.Series(data=stages, index=stages, name="stage")
            df = pd.concat([s, df], axis=1)
        else:
            # convert index to "stage" column
            df = df.reset_index().rename(columns={"index": "stage"})
            df["stage"] = df["stage"].astype("int16")

        # Fill missing values with column-specific defaults
        df = df.fillna(fill_values)

        return df

    def to_parquet(self, output_file: Union[str, Path], cost_items=None) -> None:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )

        output_dir.mkdir(parents=True, exist_ok=True)
        output_file = output_dir / output_file

        df = self.to_dataframe(cost_items)
        if df.empty:
            return

        compression = self.options.get("compression", "zstd")
        df.to_parquet(output_file, index=False, compression=compression)
