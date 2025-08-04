# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from pathlib import Path
from typing import Any, Dict, List, Optional
import typing

from .base_writer import BaseWriter
from .central_parser import CentralParser
from .bus_parser import BusParser
from .aflce_parser import AflceParser
from .aflce_writer import AflceWriter
from .block_parser import BlockParser


class GeneratorProfileWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        block_parser: Optional[BlockParser] = None,
        bus_parser: Optional[BusParser] = None,
        aflce_parser: Optional[AflceParser] = None,
        scenarios: Optional[List[Dict[str, Any]]] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser)
        self.block_parser = block_parser
        self.bus_parser = bus_parser
        self.aflce_parser = aflce_parser
        self.scenarios = scenarios
        self.options = options if options is not None else {}

    @property
    def central_parser(self) -> CentralParser:
        """Get the central parser instance."""
        return typing.cast(CentralParser, self.parser)

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = (
                self.central_parser.centrals_of_type["pasada"]
                if self.central_parser
                else []
            )
        if not items:
            return []

        json_profiles = []
        for central in items:
            # Skip centrals without a bus or with bus 0
            bus_number = central.get("bus", -1)
            if bus_number <= 0:
                continue

            if self.bus_parser:
                bus = self.bus_parser.get_bus_by_number(bus_number)
                if bus is None or bus.get("number", -1) <= 0:
                    print(
                        f"Skipping central {central['name']} with invalid bus {bus_number}."
                    )
                    continue

            central_name = central["name"]
            central_number = central["number"]

            aflce = (
                self.aflce_parser.get_item_by_name(central_name)
                if self.aflce_parser
                else None
            )
            afluent = central.get("afluent", 0.0) if aflce is None else "afluent"

            if isinstance(afluent, float) and afluent <= 0.0:
                continue

            profile = {
                "uid": central_number,
                "name": central_name,
                "generator": central_number,
                "profile": afluent,
            }
            json_profiles.append(profile)

        self._write_parquet_files()

        return json_profiles

    def _write_parquet_files(self) -> None:
        """Write demand data to Parquet file format."""
        output_dir = (
            self.options["output_dir"] / "GeneratorProfile"
            if "output_dir" in self.options
            else Path("GeneratorProfile")
        )
        output_dir.mkdir(parents=True, exist_ok=True)

        aflce_writer = AflceWriter(
            self.aflce_parser,
            self.central_parser,
            self.block_parser,
            self.scenarios,
            self.options,
        )
        aflce_writer.to_parquet(output_dir)
