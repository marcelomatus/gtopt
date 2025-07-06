#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting generator data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .generator_parser import GeneratorParser


class GeneratorWriter(BaseWriter):
    """Converts generator parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_generators()

    def __init__(self, generator_parser: GeneratorParser):
        """Initialize with a GeneratorParser instance."""
        super().__init__(generator_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert generator data to JSON array format."""
        return [
            {
                "uid": int(gen["id"]),  # Keep generator UIDs as integers
                "name": gen["name"],
                "bus": gen["bus"],
                "gcost": gen["variable_cost"],
                "capacity": gen["p_max"],
                "expcap": gen.get("pot_tm0"),  # Initial power if available
                "expmod": gen.get("afluent"),  # Inflow if available  
                "annual_capcost": None,  # Not in PLP format
                "is_battery": gen.get("is_battery", False),
            }
            for gen in self.items
        ]


if __name__ == "__main__":
    BaseWriter.main(GeneratorWriter, GeneratorParser)
