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
        json_generators = []
        for gen in self.items:
            generator = {
                "uid": int(gen["id"]),
                "name": gen["name"],
                "bus": gen["bus"],
                "gcost": float(gen["variable_cost"]),
                "capacity": float(gen["p_max"]),
                "is_battery": bool(gen.get("is_battery", False)),
                "type": gen.get("type", "unknown"),
                "efficiency": float(gen.get("efficiency", 1.0)),
                "pmin": float(gen.get("p_min", 0.0)),
            }
            
            # Add optional fields if they exist
            if "pot_tm0" in gen:
                generator["expcap"] = float(gen["pot_tm0"])
            if "afluent" in gen:
                generator["expmod"] = float(gen["afluent"])
                
            json_generators.append(generator)
            
        return json_generators


if __name__ == "__main__":
    BaseWriter.main(GeneratorWriter, GeneratorParser)
