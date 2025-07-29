# -*- coding: utf-8 -*-

"""Parser for plpbar.dat format files containing bus data.

Handles:
- File parsing and validation
- Bus data structure creation
- Bus lookup by name
"""

import re
from pathlib import Path
from typing import Any, List, Dict, Union, Optional


from .base_parser import BaseParser


class BusParser(BaseParser):
    """Parser for plpbar.dat format files containing bus data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with bus file path.

        Args:
            file_path: Path to plpbar.dat format file (str or Path)
        """
        super().__init__(file_path)
        self.bus_num_map: Dict[int, int] = {}

    @property
    def buses(self) -> List[Dict[str, Union[str, float, bool]]]:
        """Return the parsed buses structure."""
        return self.get_all()

    @property
    def num_buses(self) -> int:
        """Return the number of buses in the file."""
        return len(self.buses)

    def parse(self) -> None:
        """Parse the bus file and populate the buses structure."""
        self.validate_file()
        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("File is empty")

        idx = 0
        num_buses = self._parse_int(lines[idx])
        idx += 1

        for _ in range(num_buses):
            # Parse bus line with format: "number 'name' [voltage]"
            line = lines[idx].strip()
            idx += 1

            # Split into number and quoted name (voltage is optional)
            parts = line.split(maxsplit=1)
            if len(parts) < 2:
                raise ValueError(f"Invalid bus entry at line {idx}")

            bus_num = self._parse_int(parts[0])
            name = parts[1].strip("'")

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
                    # Default to 1 if no voltage found in name
                    voltage = 1.0

            if bus_num > 0:
                self._append({"number": bus_num, "name": name, "voltage": voltage})
                self.bus_num_map[bus_num] = self.num_buses - 1

    def get_bus_by_number(self, number: int) -> Optional[Dict[str, Any]]:
        """Get bus by bus number."""
        return self.get_item_by_number(number)

    def get_bus_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get bus by bus name."""
        if not isinstance(name, str):
            raise TypeError("name must be str")
        return self.get_item_by_name(name)
