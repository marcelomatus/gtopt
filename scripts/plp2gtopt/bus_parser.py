#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpbar.dat format files containing bus data.

Handles:
- File parsing and validation
- Bus data structure creation
- Bus lookup by name
"""

import re
import sys
from pathlib import Path
from typing import Any, Optional, List, Dict, Union


from .base_parser import BaseParser


class BusParser(BaseParser):
    """Parser for plpbar.dat format files containing bus data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with bus file path.

        Args:
            file_path: Path to plpbar.dat format file (str or Path)
        """
        super().__init__(file_path)
        self._data: List[Dict[str, Any]] = []
        self.buses: List[Dict[str, Any]] = self._data  # Alias for _data
        self.num_buses: int = 0

    def parse(self) -> None:
        """Parse the bus file and populate the buses structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        self.validate_file()
        lines = self._read_non_empty_lines()

        idx = 0
        self.num_buses = self._parse_int(lines[idx])
        idx += 1

        for _ in range(self.num_buses):
            # Parse bus line with format: "number 'name' [voltage]"
            bus_line = lines[idx].strip()
            idx += 1

            # Split into number and quoted name (voltage is optional)
            parts = bus_line.split(maxsplit=1)
            if len(parts) < 2:
                raise ValueError(f"Invalid bus entry at line {idx}")

            bus_num = int(parts[0])
            name = parts[1].strip("'").split("#")[0].strip()

            # Try to extract voltage from name (handles various patterns)
            voltage_match = re.search(
                r"(\d+)(?:kV|KV)?(?:[-_].*)?$", name, re.IGNORECASE
            )
            if voltage_match:
                voltage = float(voltage_match.group(1))
            else:
                # Try alternative patterns like 'KV220' format
                voltage_match = re.search(r"(?:kV|KV)(\d+)", name, re.IGNORECASE)
                if voltage_match:
                    voltage = float(voltage_match.group(1))
                else:
                    # Default to 0 if no voltage found in name
                    voltage = 0.0

            self._data.append({"number": bus_num, "name": name, "voltage": voltage})

    def get_buses(self) -> list[dict[str, Any]]:
        """Return the parsed buses structure."""
        return self.buses

    def get_num_buses(self) -> int:
        """Return the number of buses in the file."""
        return self.num_buses

    def get_bus_by_name(self, name: str) -> dict[str, Any] | None:
        """Get bus data for a specific bus name."""
        for bus in self.buses:
            if bus["name"] == name:
                return bus
        return None


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for bus file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        int: Exit status (0 for success)
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpbar.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Bus file not found: {input_path}")

        parser = BusParser(str(input_path))
        parser.parse()

        print(f"\nBus File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total buses: {parser.get_num_buses()}")

        buses = parser.get_buses()
        for bus in buses:
            print(f"\nBus: {bus['name']}")
            print(f"  Number: {bus['number']}")
            print(f"  Voltage: {bus['voltage']} kV")

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
