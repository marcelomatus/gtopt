#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Writer for converting bus data to JSON format."""

from typing import List, Dict, Any
from pathlib import Path
import json
from .bus_parser import BusParser


class BusWriter:
    """Converts bus parser data to JSON format used by GTOPT.

    Handles:
    - Converting bus data to JSON array format
    - Writing to output files
    - Maintaining format consistency with system_c0.json
    """

    def __init__(self, bus_parser: BusParser):
        """Initialize with a BusParser instance.

        Args:
            bus_parser: BusParser containing parsed bus data
        """
        self.bus_parser = bus_parser
        self.buses = bus_parser.get_buses()

    def to_json_array(self) -> List[Dict[str, Any]]:
        """Convert bus data to JSON array format.

        Returns:
            List of bus dictionaries in GTOPT JSON format
        """
        json_buses = []
        for bus in self.buses:
            json_bus = {
                "uid": bus["number"],
                "name": bus["name"],
                "voltage": bus["voltage"],
            }
            json_buses.append(json_bus)
        return json_buses

    def write_to_file(self, output_path: Path) -> None:
        """Write bus data to JSON file.

        Args:
            output_path: Path to output JSON file

        Raises:
            IOError: If file writing fails
        """
        json_data = self.to_json_array()
        try:
            with open(output_path, "w", encoding="utf-8") as f:
                json.dump(json_data, f, indent=4, ensure_ascii=False)
        except IOError as e:
            raise IOError(f"Failed to write bus JSON: {str(e)}") from e

    @staticmethod
    def from_bus_file(bus_file: Path) -> "BusWriter":
        """Create BusWriter directly from bus file.

        Args:
            bus_file: Path to plpbar.dat file

        Returns:
            BusWriter instance

        Raises:
            FileNotFoundError: If bus file doesn't exist
        """
        parser = BusParser(bus_file)
        parser.parse()
        return BusWriter(parser)


if __name__ == "__main__":
    import sys
    from pathlib import Path

    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.plpbar.dat> <output.json>")
        sys.exit(1)

    input_file = Path(sys.argv[1])
    output_file = Path(sys.argv[2])

    try:
        writer = BusWriter.from_bus_file(input_file)
        writer.write_to_file(output_file)
        print(f"Successfully wrote bus data to {output_file}")
    except Exception as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        sys.exit(1)
