# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data.

Handles:
- File parsing and validation
- Demand data structure creation
- Bus demand lookup
"""

import numpy as np

from .base_parser import BaseParser


class DemandParser(BaseParser):
    """Parser for plpdem.dat format files containing bus demand data."""

    @property
    def demands(self):
        """Return the demand entries."""
        return self.get_all()

    @property
    def num_demands(self) -> int:
        """Return the number of demand entries in the file."""
        return len(self.demands)

    def parse(self) -> None:
        """Parse the demand file and populate the demands structure."""
        self.validate_file()

        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The demand file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num_bars = self._parse_int(lines[idx])

            for dem_number in range(1, num_bars + 1):
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
                demand = {
                    "number": dem_number,
                    "name": name,
                    "blocks": blocks,
                    "values": values,
                }
                self._append(demand)

        finally:
            lines.clear()
            del lines

    def get_demand_by_name(self, name):
        """Get demand data for a specific bus name."""
        return self.get_item_by_name(name)
