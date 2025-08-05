# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional, TypedDict
import typing

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .cost_parser import CostParser
from .stage_parser import StageParser
from .bus_parser import BusParser
from .mance_parser import ManceParser
from .cost_writer import CostWriter
from .mance_writer import ManceWriter
from .block_parser import BlockParser


class Generator(TypedDict):
    """Represents a generator in the system."""

    uid: int
    name: str
    bus: int
    gcost: float | str
    capacity: float
    efficiency: float
    pmax: float | str
    pmin: float | str
    type: str


class CentralWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        stage_parser: Optional[StageParser] = None,
        block_parser: Optional[BlockParser] = None,
        cost_parser: Optional[CostParser] = None,
        bus_parser: Optional[BusParser] = None,
        mance_parser: Optional[ManceParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser, options)
        self.stage_parser = stage_parser
        self.block_parser = block_parser
        self.cost_parser = cost_parser
        self.bus_parser = bus_parser
        self.mance_parser = mance_parser

    @property
    def central_parser(self) -> CentralParser:
        """Get the central parser instance."""
        return typing.cast(CentralParser, self.parser)

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = self.items
        if not items:
            return []

        parquet_cols = self._write_parquet_files()

        json_centrals: List[Generator] = []
        for central in items:
            central_name = central["name"]
            central_number = central["number"]

            # skip centrals that are "falla" type
            if central["type"] == "falla":
                # Falla centrals are not included in the output
                continue

            # Skip centrals without a bus or with bus 0
            bus_number = central.get("bus", -1)
            if bus_number <= 0:
                continue

            if self.bus_parser:
                bus = self.bus_parser.get_bus_by_number(bus_number)
                if bus is None or bus["number"] != bus_number:
                    print(
                        f"Skipping central {central_name} with invalid bus {bus_number}."
                    )
                    continue

            # lookup for cols in parquet files
            pcol_name = self.pcol_name(central_name, central_number)
            gcost = "gcost" if pcol_name in parquet_cols["gcost"] else central["gcost"]
            pmin = "pmin" if pcol_name in parquet_cols["pmin"] else central["pmin"]
            pmax = "pmax" if pcol_name in parquet_cols["pmax"] else central["pmax"]

            generator: Generator = {
                "uid": central_number,
                "name": central_name,
                "bus": bus_number,
                "gcost": gcost,
                "capacity": float(central["pmax"]),
                "efficiency": float(central["efficiency"]),
                "pmax": pmax,
                "pmin": pmin,
                "type": central.get("type", "unknown"),
            }
            json_centrals.append(generator)

        return typing.cast(List[Dict[str, Any]], json_centrals)

    def _write_parquet_files(self) -> Dict[str, List[str]]:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "Generator"
            if "output_dir" in self.options
            else Path("Generator")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        cost_writer = CostWriter(
            self.cost_parser,
            self.central_parser,
            self.stage_parser,
            self.options,
        )
        cost_cols = cost_writer.to_parquet(output_dir)

        mance_writer = ManceWriter(
            self.mance_parser, self.central_parser, self.block_parser, self.options
        )
        mance_cols = mance_writer.to_parquet(output_dir)

        mcols = {}
        for d in cost_cols, mance_cols:
            for key, value in d.items():
                mcols[key] = value

        return mcols
