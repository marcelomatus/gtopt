# -*- coding: utf-8 -*-

"""Parser for plpmance.dat format files containing maintenance data.

Handles:
- File parsing and validation
- Maintenance data structure creation
- Maintenance lookup by name
"""

from pathlib import Path
from typing import Union, Dict, List
import numpy as np

from .base_parser import BaseParser


class ManceParser(BaseParser):
    """Parser for plpmance.dat format files containing maintenance data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with maintenance file path."""
        super().__init__(file_path)

    @property
    def mances(self) -> List[Dict]:
        """Return the maintenance entries."""
        return self.get_all()

    @property
    def num_mances(self) -> int:
        """Return the number of maintenance entries in the file."""
        return len(self.mances)

    def parse(self) -> None:
        """Parse the maintenance file and populate the data structure."""
        self.validate_file()

        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The maintenance file is empty or malformed.")

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

                # Initialize numpy arrays
                blocks = np.empty(num_blocks, dtype=np.int16)
                p_min = np.empty(num_blocks, dtype=np.float32)
                p_max = np.empty(num_blocks, dtype=np.float32)

                # Parse maintenance entries
                for i in range(num_blocks):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 5:
                        raise ValueError(f"Invalid maintenance entry at line {idx+1}")

                    blocks[i] = self._parse_int(parts[1])
                    p_min[i] = self._parse_float(parts[3])
                    p_max[i] = self._parse_float(parts[4])

                # Store complete data
                mance = {
                    "name": name,
                    "blocks": blocks,
                    "p_min": p_min,
                    "p_max": p_max,
                }
                self._append(mance)

        finally:
            lines.clear()
            del lines

    def get_mance_by_name(self, name: str) -> Union[Dict, None]:
        """Get maintenance data for a specific central name."""
        return self.get_item_by_name(name)
