# -*- coding: utf-8 -*-

"""Parser for plpmance.dat format files containing maintenance data.

Handles:
- File parsing and validation
- Maintenance data structure creation
- Maintenance lookup by name
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class ManceParser(BaseParser):
    """Parser for plpmance.dat format files containing maintenance data."""

    @property
    def mances(self) -> List[Dict[str, Any]]:
        """Return the maintenance entries."""
        return self.get_all()

    @property
    def num_mances(self) -> int:
        """Return the number of maintenance entries in the file."""
        return len(self.mances)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the maintenance file and populate the data structure."""
        self.validate_file()

        lines: list[str] = []
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
                if num_blocks <= 0:
                    continue

                # Vectorized parse: cols 1=block, 3=pmin, 4=pmax
                next_idx, cols = self._parse_numeric_block(
                    lines,
                    idx + 1,
                    num_blocks,
                    int_cols=(1,),
                    float_cols=(3, 4),
                )
                idx = next_idx - 1

                # Store complete data
                mance = {
                    "name": name,
                    "block": cols[1],
                    "pmin": cols[3],
                    "pmax": cols[4],
                }
                self._append(mance)

        finally:
            lines.clear()

    def get_mance_by_name(self, name: str) -> Dict[str, Any] | None:
        """Get maintenance data for a specific central name."""
        return self.get_item_by_name(name)
