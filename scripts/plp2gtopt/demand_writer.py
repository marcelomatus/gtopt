#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting demand data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .demand_parser import DemandParser


class DemandWriter(BaseWriter):
    """Converts demand parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_demands()

    def __init__(self, demand_parser: DemandParser):
        """Initialize with a DemandParser instance."""
        super().__init__(demand_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert demand data to JSON array format."""
        return [
            {
                "uid": hash(demand["name"])
                & 0x7FFFFFFF,  # Generate stable positive integer UID from name
                "name": demand["name"],
                "bus": demand["name"],  # Using name as bus ID
                "lmax": "lmax",  # Default value from system_c0.json
                "capacity": 0.0,  # Default value from system_c0.json
                "expcap": None,  # Not in PLP format
                "expmod": None,  # Not in PLP format
                "annual_capcost": None,  # Not in PLP format
            }
            for demand in self.items
        ]


if __name__ == "__main__":
    BaseWriter.main(DemandWriter, DemandParser)
