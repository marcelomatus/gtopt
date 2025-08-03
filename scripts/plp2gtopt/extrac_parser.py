# -*- coding: utf-8 -*-

"""Parser for plpextrac.dat format files containing extraction data.

Handles:
- File parsing and validation
- Extraction data structure creation
- Extraction lookup by name
"""

from typing import Any, Dict, List

from .base_parser import BaseParser


class ExtracParser(BaseParser):
    """Parser for plpextrac.dat format files containing extraction data."""

    @property
    def extracs(self) -> List[Dict[str, Any]]:
        """Return the extraction entries."""
        return self.get_all()

    @property
    def num_extracs(self) -> int:
        """Return the number of extraction entries in the file."""
        return len(self.extracs)

    def parse(self) -> None:
        """Parse the extraction file and populate the data structure."""
        self.validate_file()

        lines = []
        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The extraction file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num_centrals = self._parse_int(lines[idx])

            for _ in range(num_centrals):
                # Get extraction central name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'")

                # Get max extraction value
                idx = self._next_idx(idx, lines)
                max_extrac = self._parse_float(lines[idx])

                # Get downstream central name
                idx = self._next_idx(idx, lines)
                downstream = lines[idx].strip("'")

                # Store complete data
                extrac = {
                    "name": name,
                    "max_extrac": max_extrac,
                    "downstream": downstream,
                }
                self._append(extrac)

        finally:
            lines.clear()

    def get_extrac_by_name(self, name: str) -> Dict[str, Any] | None:
        """Get extraction data for a specific central name."""
        return self.get_item_by_name(name)
