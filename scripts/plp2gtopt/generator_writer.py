#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting generator data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .generator_parser import CentralParser


class CentralWriter(BaseWriter):
    """Converts generator parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_generators()

    def __init__(self, generator_parser: CentralParser):
        """Initialize with a GeneratorParser instance."""
        if not hasattr(generator_parser, "get_generators"):
            raise ValueError("Parser must implement get_generators()")
        super().__init__(generator_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert generator data to JSON array format."""
        json_generators = []
        for gen in self.items:
            # Skip generators without a bus or with bus 0
            if gen["bus"] == 0:
                continue

            generator = {
                "uid": gen["number"],
                "name": gen["name"],
                "bus": gen["bus"],
                "gcost": float(gen.get("variable_cost", 0.0)),
                "capacity": float(gen.get("p_max", 0)),
                "efficiency": float(gen.get("efficiency", 1.0)),
                "pmax": float(gen.get("p_max", 0.0)),
                "pmin": float(gen.get("p_min", 0.0)),
                "type": gen.get("type", "unknown"),
            }

            json_generators.append(generator)

        return json_generators


if __name__ == "__main__":
    BaseWriter.main(CentralWriter, CentralParser)
