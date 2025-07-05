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
                "uid": int(gen["id"]),  # Convert string ID to integer
                "name": gen["name"],
                "bus": gen["bus"],
                "gcost": gen["variable_cost"],
                "capacity": gen["p_max"],
                "expcap": None,  # Not in PLP format
                "expmod": None,  # Not in PLP format
                "annual_capcost": None,  # Not in PLP format
            }
            for gen in self.items
        ]


if __name__ == "__main__":
    BaseWriter.main(GeneratorWriter, GeneratorParser)
