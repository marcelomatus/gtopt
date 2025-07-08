#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .central_parser import CentralParser


class CentralWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_centrals()

    def __init__(self, central_parser: CentralParser):
        """Initialize with a CentralParser instance."""
        if not hasattr(central_parser, "get_centrals"):
            raise ValueError("Parser must implement get_centrals()")
        super().__init__(central_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        json_centrals = []
        for gen in self.items:
            # Skip centrals without a bus or with bus 0
            if gen["bus"] == 0:
                continue

            central = {
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

            json_centrals.append(central)

        return json_centrals


if __name__ == "__main__":
    BaseWriter.main(CentralWriter, CentralParser)
