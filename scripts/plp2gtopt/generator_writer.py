#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting generator data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .generator_parser import GeneratorParser


class GeneratorWriter(BaseWriter):
    """Converts generator parser data to JSON format used by GTOPT."""

    def __init__(self, generator_parser: GeneratorParser):
        """Initialize with a GeneratorParser instance."""
        super().__init__(generator_parser)
        self.generators = generator_parser.get_generators()

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert generator data to JSON array format."""
        return [
            {
                "uid": gen["id"],
                "name": gen["name"],
                "bus": gen["bus"],
                "gcost": gen["variable_cost"],
                "capacity": gen["p_max"],
                "expcap": None,  # Not in PLP format
                "expmod": None,  # Not in PLP format
                "annual_capcost": None,  # Not in PLP format
            }
            for gen in self.generators
        ]


if __name__ == "__main__":
    BaseWriter.main(GeneratorWriter, GeneratorParser)
