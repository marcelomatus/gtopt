# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional


from .base_writer import BaseWriter
from .central_parser import CentralParser
from .cost_parser import CostParser
from .stage_parser import StageParser
from .bus_parser import BusParser
from .mance_parser import ManceParser
from .cost_writer import CostWriter
from .mance_writer import ManceWriter


class CentralWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        stage_parser: Optional[StageParser] = None,
        cost_parser: Optional[CostParser] = None,
        bus_parser: Optional[BusParser] = None,
        mance_parser: Optional[ManceParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
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

    def process_central_series(self, series):
        """Process series to include block and stage information."""
        if not series:
            return

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
                (cen.get("p_min", 0.0), cen.get("p_max", 0.0))
                if mance is None
                else ("pmin", "pmax")
            )

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

        self._write_parquet_files()

        return json_centrals

    def _write_parquet_files(self) -> None:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        cost_writer = CostWriter(
            self.cost_parser, self.parser, self.stage_parser, self.options
        )
        cost_writer.to_parquet(output_dir)

        mance_writer = ManceWriter(self.mance_parser, self.parser, self.options)
        mance_writer.to_parquet(output_dir)
