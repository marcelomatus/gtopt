# -*- coding: utf-8 -*-

"""Parser for plpmanli.dat format files containing line maintenance data.

Handles:
- File parsing and validation
- Line maintenance data structure creation
- Maintenance lookup by line name
"""

from typing import Union, Dict, List
import numpy as np

from .base_parser import BaseParser


class ManliParser(BaseParser):
    """Parser for plpmanli.dat format files containing line maintenance data."""

    @property
    def manlis(self) -> List[Dict]:
        """Return the maintenance entries."""
        return self.get_all()

    @property
    def num_manlis(self) -> int:
        """Return the number of maintenance entries in the file."""
        return len(self.manlis)

    def parse(self) -> None:
        """Parse the maintenance file and populate the data structure."""
        self.validate_file()

        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The line maintenance file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num_lines = self._parse_int(lines[idx])

            for _ in range(num_lines):
                # Get line name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'")

                # Get number of blocks
                idx = self._next_idx(idx, lines)
                parts = lines[idx].split()
                num_blocks = self._parse_int(parts[0])
                if num_blocks <= 0:
                    continue

                # Initialize numpy arrays
                blocks = np.empty(num_blocks, dtype=np.int16)
                p_max_ab = np.empty(num_blocks, dtype=np.float64)
                p_max_ba = np.empty(num_blocks, dtype=np.float64)
                operational = np.empty(num_blocks, dtype=np.int8)

                # Parse maintenance entries
                for i in range(num_blocks):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 4:
                        raise ValueError(f"Invalid maintenance entry at line {idx+1}")

                    blocks[i] = self._parse_int(parts[0])  # Block number
                    p_max_ab[i] = self._parse_float(parts[1])  # Max flow AB
                    p_max_ba[i] = self._parse_float(parts[2])  # Max flow BA
                    operational[i] = np.int16(parts[3] == "T")  # Operational status

                # Store complete data
                manli = {
                    "name": name,
                    "block": blocks,
                    "tmax_ab": p_max_ab,
                    "tmax_ba": p_max_ba,
                    "operational": operational,
                }

                self._append(manli)

        finally:
            lines.clear()

    def get_manli_by_name(self, name: str) -> Union[Dict, None]:
        """Get maintenance data for a specific line name."""
        return self.get_item_by_name(name)
