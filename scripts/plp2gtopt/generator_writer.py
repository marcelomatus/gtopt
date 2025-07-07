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
        if not hasattr(generator_parser, "get_generators"):
            raise ValueError("Parser must implement get_generators()")
        super().__init__(generator_parser)
        # Ensure all generators have required fields
        for gen in self.items:
            gen.setdefault("p_max", gen.get("capacity", 0.0))
            gen.setdefault("bus", gen["id"])

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert generator data to JSON array format."""
        json_generators = []
        for gen in self.items:
            generator = {
                "uid": int(gen["id"]),
                "name": gen["name"],
                "bus": str(gen.get("bus", gen["id"])),  # Fallback to id if bus missing
                "gcost": float(gen.get("variable_cost", 0.0)),
                "capacity": float(gen.get("p_max", gen.get("capacity", 0.0))),
                "is_battery": gen.get("type", "") == "bateria",
                "expcap": float(gen["pot_tm0"]) if "pot_tm0" in gen else None,
                "expmod": float(gen["afluent"]) if "afluent" in gen else None,
                "annual_capcost": None,
                "type": gen.get("type", "unknown"),
                "efficiency": float(gen.get("efficiency", 1.0)),
                "pmin": float(gen.get("p_min", 0.0)),
            }

            json_generators.append(generator)

        return json_generators


if __name__ == "__main__":
    BaseWriter.main(GeneratorWriter, GeneratorParser)
