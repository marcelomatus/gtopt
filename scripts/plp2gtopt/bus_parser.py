#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpbar.dat format files containing bus data.

Handles:
- File parsing and validation
- Bus data structure creation
- Bus lookup by name
"""

import sys
from pathlib import Path
from typing import Any, Optional, List, Dict, Union


class BusParser:
    """Parser for plpbar.dat format files containing bus data.

    Attributes:
        file_path: Path to the bus file
        buses: List of parsed bus entries
        num_buses: Number of buses in the file
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with bus file path.

        Args:
            file_path: Path to plpbar.dat format file (str or Path)
        """
        self.file_path = Path(file_path) if isinstance(file_path, str) else file_path
        self.buses: List[Dict[str, Any]] = []
        self.num_buses: int = 0

    def parse(self) -> None:
        """Parse the bus file and populate the buses structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        if not self.file_path.exists():
            raise FileNotFoundError(f"Bus file not found: {self.file_path}")

        with open(self.file_path, "r", encoding="utf-8") as f:
            # Skip initial comments and empty lines
            lines = []
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    lines.append(line)

        idx = 0
        self.num_buses = int(lines[idx])
        idx += 1

        for _ in range(self.num_buses):
            # Parse bus line with format: "number 'name'"
            bus_line = lines[idx].strip()
            idx += 1

            # Split into number and quoted name
            parts = bus_line.split(maxsplit=1)
            if len(parts) < 2:
                raise ValueError(f"Invalid bus entry at line {idx}")
            bus_num = int(parts[0])
            name = parts[1].strip("'").split("#")[0].strip()

            self.buses.append({
                "number": bus_num,
                "name": name
            })

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

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
