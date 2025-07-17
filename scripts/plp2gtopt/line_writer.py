# -*- coding: utf-8 -*-

"""Writer for converting line data to JSON format."""

from typing import Any, Dict, List, Optional
from .base_writer import BaseWriter
from .line_parser import LineParser


class LineWriter(BaseWriter):
    """Converts line parser data to JSON format used by GTOPT."""

    def __init__(self, line_parser: Optional[LineParser] = None):
        """Initialize with a LineParser instance."""
        super().__init__(line_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert line data to JSON array format."""
        if items is None:
            items = self.items
        return [
            {
                "uid": line["number"],
                "name": line["name"],
                "active": int(line["active"]),
                "bus_a": line["bus_a"],
                "bus_b": line["bus_b"],
                "resistance": line["r"],
                "reactance": line["x"],
                "tmax": line["fmax_ab"],
                "tmin": -line["fmax_ba"],
                "voltage": line["voltage"],
                **({"is_hvdc": 1} if "hdvc" in line else {}),
            }
            for line in items
        ]
