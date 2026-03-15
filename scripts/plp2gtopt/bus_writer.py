# -*- coding: utf-8 -*-

"""Writer for converting bus data to JSON format."""

from typing import Any, Dict, List, Optional, TypedDict, cast
from .base_writer import BaseWriter
from .bus_parser import BusParser


class Bus(TypedDict):
    """Represents a bus in the system."""

    uid: int
    name: str
    voltage: float


class BusWriter(BaseWriter):
    """Converts bus parser data to JSON format used by GTOPT."""

    def __init__(
        self,
        bus_parser: Optional[BusParser] = None,
        options: Optional[Dict[str, Any]] = None,
    ):
        """Initialize with a BusParser instance."""
        super().__init__(bus_parser, options)

    def to_json_array(self, items=None) -> List[Dict[str, Any]]:
        """Convert bus data to JSON array format.

        In PLP the first bus is always used as the DC-OPF reference bus
        (angle variable th1 is fixed to zero via [0, 0] bounds in
        GenPDAngFO).  We therefore set ``reference_theta: 0.0`` on the
        first bus so that gtopt knows which bus angle to pin.
        """
        if items is None:
            items = self.items or []
        json_buses: List[Bus] = [
            {
                "uid": bus["number"],
                "name": bus["name"],
                "voltage": bus["voltage"],
            }
            for bus in items
        ]
        result = cast(List[Dict[str, Any]], json_buses)
        # Mark the first bus as the reference bus (theta = 0).
        # PLP always fixes bus 1 as the angle reference (GenPDAngFO).
        if result:
            result[0]["reference_theta"] = 0.0
        return result
