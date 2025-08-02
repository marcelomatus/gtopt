# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from typing import Any, Dict, List, Optional
import typing

from .base_writer import BaseWriter
from .central_parser import CentralParser


class JunctionWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        central_parser: Optional[CentralParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ) -> None:
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser)
        self.options = options if options is not None else {}

        self.num_waterways = 0

    @property
    def central_parser(self) -> CentralParser:
        """Get the central parser instance."""
        return typing.cast(CentralParser, self.parser)

    def json_waterway(self, central_name, junction_a, junction_b):
        """Add waterway."""
        if junction_a == 0 or junction_b == 0:
            return None

        self.num_waterways += 1
        return {
            "uid": self.num_waterways,
            "name": f"{central_name}_{junction_a}_{junction_b}",
            "junction_a": junction_a,
            "junction_b": junction_b,
        }

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = (
                self.central_parser.centrals_of_type["embalse"]
                + self.central_parser.centrals_of_type["serie"]
                if self.central_parser
                else []
            )
        if not items:
            return []

        json_junctions = []
        json_waterways = []
        for central in items:
            if central["type"] != "serie" or central["type"] != "embalse":
                continue

            central_name = central["name"]
            central_number = central["number"]

            wway_hid = self.json_waterway(
                central_name, central_number, central["ser_hid"]
            )
            if wway_hid:
                json_waterways.append(wway_hid)

            wway_ver = self.json_waterway(
                central_name, central_number, central["ser_ver"]
            )
            if wway_ver:
                json_waterways.append(wway_ver)

            drain = wway_hid is None or wway_ver is None

            junction = {
                "uid": central_number,
                "name": central_name,
                "drain": int(drain),
            }
            json_junctions.append(junction)

        return (json_junctions, json_waterways)
