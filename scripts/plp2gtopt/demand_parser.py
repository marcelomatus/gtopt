# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data.

Handles:
- File parsing and validation
- Demand data structure creation
- Bus demand lookup
"""

from pathlib import Path
from typing import Union
import numpy as np

from .base_parser import BaseParser


class DemandParser(BaseParser):
    """Parser for plpdem.dat format files containing bus demand data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with demand file path.

        Args:
            file_path: Path to plpdem.dat format file (str or Path)
        """
        super().__init__(file_path)

    @property
    def demands(self):
        """Return the demand entries."""
        return self.get_all()

    @property
    def num_demands(self) -> int:
        """Return the number of demand entries in the file."""
        return len(self.demands)

    def parse(self) -> None:
        """Parse the demand file and populate the demands structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed

        Example:
            >>> parser = DemandParser("plpdem.dat")
            >>> parser.parse()
            >>> demands = parser.get_demands()
            >>> len(demands)
            2
        """
        self.validate_file()

        try:
            lines = self._read_non_empty_lines()

            idx = self._next_idx(-1, lines)
            num_bars = self._parse_int(lines[idx])

            for bus_number in range(1, num_bars + 1):
                # Get bus name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'").split()[0]

                # Get number of demand entries
                idx = self._next_idx(idx, lines)
                num_blocks = self._parse_int(lines[idx].strip().split()[0])

                # Initialize numpy arrays for this bus
                blocks = np.empty(num_blocks, dtype=np.int16)
                values = np.empty(num_blocks, dtype=np.float64)

                # Parse demand entries
                for i in range(num_blocks):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 3:
                        raise ValueError(f"Invalid demand entry at line {idx+1}")

                    blocks[i] = self._parse_int(parts[1])  # Block number
                    values[i] = self._parse_float(parts[2])  # Demand value

                # Store complete data for this bus
                self._append(
                    {
                        "number": bus_number,
                        "name": name,
                        "blocks": blocks,
                        "values": values,
                    }
                )

        finally:
            lines.clear()
            del lines

    def get_demand_by_name(self, name):
        """Get demand data for a specific bus name."""
        return self.get_item_by_name(name)
