# -*- coding: utf-8 -*-

"""Writer for converting bus data to JSON format."""

from typing import Any, Dict, List, Optional
from .base_writer import BaseWriter
from .bus_parser import BusParser


class BusWriter(BaseWriter):
    """Converts bus parser data to JSON format used by GTOPT."""

    def __init__(self, bus_parser: Optional[BusParser] = None):
        """Initialize with a BusParser instance."""
        super().__init__(bus_parser)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert bus data to JSON array format."""
        if items is None:
            items = self.items
        return [
            {
                "uid": bus["number"],
                "name": bus["name"],
                "voltage": bus["voltage"],
            }
            for bus in items
        ]
