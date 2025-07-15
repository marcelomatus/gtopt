# -*- coding: utf-8 -*-

"""Parser for plpmance.dat format files containing maintenance data."""

from pathlib import Path
from typing import Union, Dict, List
import numpy as np

from .base_parser import BaseParser


class ManceParser(BaseParser):
    """Parser for plpmance.dat format files containing maintenance data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with maintenance file path."""
        super().__init__(file_path)
        self._data: List[Dict] = []

    @property
    def maintenances(self) -> List[Dict]:
        """Return the maintenance entries."""
        return self._data

    @property
    def num_maintenances(self) -> int:
        """Return the number of maintenance entries in the file."""
        return len(self._data)

    def parse(self) -> None:
        """Parse the maintenance file and populate the data structure."""
        self.validate_file()

        try:
            lines = self._read_non_empty_lines()
            idx = self._next_idx(-1, lines)
            num_centrals = self._parse_int(lines[idx])

            for _ in range(num_centrals):
                # Get central name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'")

                # Get number of blocks and intervals
                idx = self._next_idx(idx, lines)
                parts = lines[idx].split()
                num_blocks = self._parse_int(parts[0])
                num_intervals = self._parse_int(parts[1]) if len(parts) > 1 else 1

                # Initialize numpy arrays
                months = np.empty(num_blocks, dtype=np.int8)
                blocks = np.empty(num_blocks, dtype=np.int16)
                p_min = np.empty(num_blocks, dtype=np.float32)
                p_max = np.empty(num_blocks, dtype=np.float32)

                # Parse maintenance entries
                for i in range(num_blocks):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 5:
                        raise ValueError(f"Invalid maintenance entry at line {idx+1}")

                    months[i] = self._parse_int(parts[0])
                    blocks[i] = self._parse_int(parts[1])
                    p_min[i] = self._parse_float(parts[3])
                    p_max[i] = self._parse_float(parts[4])

                # Store complete data
                maint = {
                    "name": name,
                    "months": months,
                    "blocks": blocks,
                    "p_min": p_min,
                    "p_max": p_max
                }
                self._append(maint)

        finally:
            lines.clear()
            del lines

    def get_maintenance_by_name(self, name: str) -> Union[Dict, None]:
        """Get maintenance data for a specific central name."""
        return self.get_item_by_name(name)
