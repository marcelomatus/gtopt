#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting line data to JSON format."""

from typing import Any, Dict, List
from .base_writer import BaseWriter
from .line_parser import LineParser


class LineWriter(BaseWriter):
    """Converts line parser data to JSON format used by GTOPT."""

    def _get_items(self) -> List[Dict[str, Any]]:
        return self.parser.get_lines()

    def __init__(self, line_parser: LineParser):
        """Initialize with a LineParser instance."""
        super().__init__(line_parser)

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert line data to JSON array format."""
        return [
            {
                "uid": line["number"],
                "name": line["name"],
                "active": line["active"],
                "bus_a": line["bus_a"],
                "bus_b": line["bus_b"],
                "resistance": line["resistance"],
                "reactance": line["reactance"],
                "tmax": line["tmax"],
                "tmin": line["tmin"],
                "voltage": line["voltage"],
                "has_losses": line["has_losses"],
            }
            for line in self.items
        ]


if __name__ == "__main__":
    BaseWriter.main(LineWriter, LineParser)
