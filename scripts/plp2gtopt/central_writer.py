#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting central data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .central_parser import CentralParser


class CentralWriter(BaseWriter):
    """Converts central parser data to JSON format used by GTOPT."""

    def __init__(self, central_parser: CentralParser = None):
        """Initialize with a CentralParser instance."""
        super().__init__(central_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert central data to JSON array format."""
        if items is None:
            items = self.items

        json_centrals = []
        for gen in items:
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
