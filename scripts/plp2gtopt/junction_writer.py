# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from typing import Any, Dict, List, Optional, cast
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
        self.options: Dict[str, Any] = options if options is not None else {}

        self.num_waterways: int = 0

    def create_waterway(
        self, central_name: str, central_number: int, junction: int
    ) -> Optional[Dict[str, Any]]:
        """Create a waterway dictionary if the junction is valid."""
        if junction == 0:
            return None

        self.num_waterways += 1
        return {
            "uid": self.num_waterways,
            "name": f"{central_name}_{central_number}_{junction}",
            "junction_a": central_number,
            "junction_b": junction,
        }

    def to_json_array(
        self, items: Optional[List[Dict[str, Any]]] = None
    ) -> tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = (
                self.parser.centrals_of_type.get("embalse", [])
                + self.parser.centrals_of_type.get("serie", [])
                if self.parser
                else []
            )
        if not items:
            return [], []

        central_parser = cast(CentralParser, self.parser)

        json_junctions: List[Dict[str, Any]] = []
        json_waterways: List[Dict[str, Any]] = []

        for central in items:
            central_name: str = central["name"]
            central_number: int = central["number"]

            wway_hid: Optional[Dict[str, Any]] = self.create_waterway(
                central_name, central_number, central.get("ser_hid", 0)
            )
            if wway_hid:
                json_waterways.append(wway_hid)

            wway_ver: Optional[Dict[str, Any]] = self.create_waterway(
                central_name, central_number, central.get("ser_ver", 0)
            )
            if wway_ver:
                json_waterways.append(wway_ver)

            drain: int = 0
            if wway_hid is None or wway_ver is None:
                drain = 1

            junction: Dict[str, Any] = {
                "uid": central_number,
                "name": central_name,
                "drain": drain,
            }
            json_junctions.append(junction)

        return json_junctions, json_waterways
