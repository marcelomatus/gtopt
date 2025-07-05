#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting bus data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .bus_parser import BusParser


class BusWriter(BaseWriter):
    """Converts bus parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_buses()

    def __init__(self, bus_parser: BusParser):
        """Initialize with a BusParser instance."""
        super().__init__(bus_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert bus data to JSON array format."""
        return [
            {
                "uid": bus["number"],
                "name": bus["name"], 
                "voltage": bus["voltage"],
            }
            for bus in self.buses
        ]


if __name__ == "__main__":
    BaseWriter.main(BusWriter, BusParser)
